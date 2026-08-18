#include "editor/AutosaveService.hpp"

#include <algorithm>
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

		if (!undo->HasUnsavedChanges() || undo->EditSequence() == m_lastSavedEditSeq)
		{
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
		auto write = [snapshot = std::move(snapshot), target, sceneName]() mutable
		{
			std::string toml;
			std::vector<std::byte> unusedBinary;
			app::scene::SerializeScene(snapshot, toml, unusedBinary);

			if (auto dir = io::file_util::CreateDirectories(target.parent_path()); !dir)
			{
				AE_WARN(LogCategory::App, "Autosave: could not create '{}': {}", target.parent_path().string(), dir.error().message);
				return;
			}
			if (auto written = io::file_util::WriteText(target, toml); !written)
			{
				AE_WARN(LogCategory::App, "Autosave: could not write '{}': {}", target.string(), written.error().message);
				return;
			}
			AE_INFO(LogCategory::App, "Autosaved a recovery copy of '{}' to {}", sceneName, target.string());
		};

		if (io != nullptr)
		{
			io->Submit(io::IOPriority::Background, std::move(write));
		}
		else
		{
			write(); // no executor (tests, headless): correctness beats the frame budget
		}
	}

} // namespace aether::editor
