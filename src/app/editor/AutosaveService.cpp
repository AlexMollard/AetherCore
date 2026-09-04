#include "editor/AutosaveService.hpp"

#include <system_error>
#include <system_error>
#include <utility>

#include "debug/UndoStack.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/FileUtil.hpp"
#include "io/IOThread.hpp"
#include "layers/AppLayer.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/SettingsService.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kRecoverySuffix = ".scene.toml";

	} // namespace

	void AutosaveService::Tick(app::LayerContext& context)
	{
		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			return;
		}

		// Play mutates the world every frame and Stop restores it, so autosaving during
		// Play would capture runtime state and offer it back as authored content.
		const auto* play = context.TryGet<app::PlayState>();
		if (play != nullptr && play->IsPlaying())
		{
			return;
		}

		auto* undo = context.TryGet<UndoStack>();
		auto* scenes = context.TryGet<SceneSubsystem>();
		auto* assets = context.TryGet<AssetManager>();
		if (undo == nullptr || scenes == nullptr || assets == nullptr)
		{
			return;
		}

		float intervalSeconds = 120.0f;
		if (const auto* settings = context.TryGet<SettingsService>())
		{
			intervalSeconds = settings->Get().app.autosaveSeconds;
		}
		if (intervalSeconds <= 0.0f)
		{
			return; // explicitly disabled
		}

		const auto now = std::chrono::steady_clock::now();
		if (!m_started)
		{
			m_started = true;
			m_lastSave = now;
			m_lastSavedEditSeq = undo->EditSequence();
			return;
		}

		const bool clean = !undo->HasUnsavedChanges();
		if (clean || undo->EditSequence() == m_lastSavedEditSeq)
		{
			// The document went clean while a background write was still queued - the
			// user saved (the save path discards the recovery copy), undid back to the
			// file's state, or the document was replaced. The queued copy predates that
			// moment; letting it land would recreate already-accounted-for content with
			// a mtime newer than the scene, which FindRecoverable offers right back.
			if (clean && m_recoveryWriteInFlight)
			{
				m_recoveryWriteInFlight = false;
				InvalidatePendingWrites();
			}
			return;
		}
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSave).count() < static_cast<long long>(intervalSeconds * 1000.0f))
		{
			return;
		}

		const std::string sceneName = scenes->GetCurrentScene();
		if (sceneName.empty())
		{
			return; // an unnamed scene has no file to recover into
		}

		// The ECS read has to happen here, on the game thread. Everything after it - building
		// the document tree, writing the file - is handed off, because a large scene costs
		// most of a second and this runs inside a frame.
		app::scene::SceneDescription snapshot = app::scene::CaptureScene(scenes->GetWorld(), assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());

		m_lastSave = now;
		m_lastSavedEditSeq = undo->EditSequence();

		const std::filesystem::path target = RecoveryDirectory(*project) / (sceneName + std::string(kRecoverySuffix));
		auto* io = context.TryGet<io::IoExecutor>();
		// Newest write wins: a submit supersedes any older copy still queued, and the
		// generation it captures is what lets a save or discard cancel it in flight.
		const std::uint64_t generation = ++s_recoveryWriteGeneration;
		auto write = [snapshot = std::move(snapshot), target, sceneName, generation]() mutable
		{
			const auto superseded = [generation] { return s_recoveryWriteGeneration.load() != generation; };
			if (superseded())
			{
				return;
			}

			std::string toml;
			std::vector<std::byte> unusedBinary;
			app::scene::SerializeScene(snapshot, toml, unusedBinary);

			if (auto dir = io::file_util::CreateDirectories(target.parent_path()); !dir)
			{
				AE_WARN(LogCategory::App, "Autosave: could not create '{}': {}", target.parent_path().string(), dir.error().message);
				return;
			}
			// Temp-then-rename, never a truncating write over the copy itself: a crash
			// mid-write used to leave a truncated recovery file NEWER than the scene it
			// shadows, and FindRecoverable offers exactly that (it cannot know the copy
			// is a corpse). The .tmp suffix also keeps it out of that scan.
			std::filesystem::path tempFile = target;
			tempFile += ".tmp";
			if (auto written = io::file_util::WriteText(tempFile, toml); !written)
			{
				AE_WARN(LogCategory::App, "Autosave: could not write '{}': {}", tempFile.string(), written.error().message);
				return;
			}
			if (superseded())
			{
				// A save or discard happened while this write was queued or in flight:
				// the target may already be gone, and re-creating it would resurrect
				// content the user just accounted for.
				AE_INFO(LogCategory::App, "Autosave: recovery write of '{}' skipped; superseded by a newer write, save or discard.", sceneName);
				std::error_code removeEc;
				std::filesystem::remove(tempFile, removeEc);
				return;
			}
			std::error_code renameEc;
			std::filesystem::rename(tempFile, target, renameEc);
			if (renameEc)
			{
				AE_WARN(LogCategory::App, "Autosave: could not finalize '{}': {}", target.string(), renameEc.message());
				std::error_code removeEc;
				std::filesystem::remove(tempFile, removeEc);
				return;
			}
			AE_INFO(LogCategory::App, "Autosaved a recovery copy of '{}' to {}", sceneName, target.string());
		};

		if (io != nullptr)
		{
			m_recoveryWriteInFlight = true;
			io->Submit(io::IOPriority::Background, std::move(write));
		}
		else
		{
			write(); // no executor (tests, headless): correctness beats the frame budget
		}
	}

} // namespace aether::editor
