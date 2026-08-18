// The recovery half of AutosaveService, kept in its own TU so it can be tested.
//
// Tick() needs a live editor - LayerContext, PlayState, the asset manager, the IO
// executor. These three do not: they are pure filesystem rules over a project layout,
// and they are the half where a mistake destroys work rather than merely failing to
// save it. Same split, and same reason, as RuntimeProjectSettings.
#include "editor/AutosaveService.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#include "editor/EditorProjectContext.hpp"
#include "io/FileUtil.hpp"
#include "scene/SceneSerializer.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kRecoverySuffix = ".scene.toml";

		std::filesystem::path SceneFileFor(const app::EditorProjectContext& project, const std::string& sceneName)
		{
			return project.scenesDir / (sceneName + std::string(kRecoverySuffix));
		}
	} // namespace

	std::filesystem::path AutosaveService::RecoveryDirectory(const app::EditorProjectContext& project)
	{
		// Beside the project rather than in the scenes folder: a recovery copy must never be
		// mistaken for a scene by anything that lists them, and .aether/ is already the
		// project's own private corner.
		return project.root / ".aether" / "recovery";
	}

	std::vector<RecoveredScene> AutosaveService::FindRecoverable(const app::EditorProjectContext& project)
	{
		std::vector<RecoveredScene> found;
		if (!project.IsLoaded())
		{
			return found;
		}

		const std::filesystem::path dir = RecoveryDirectory(project);
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec))
		{
			return found;
		}

		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (ec || !entry.is_regular_file(ec))
			{
				continue;
			}
			const std::string file = entry.path().filename().string();
			if (!file.ends_with(kRecoverySuffix))
			{
				continue;
			}

			RecoveredScene rec;
			rec.sceneName = file.substr(0, file.size() - kRecoverySuffix.size());
			rec.recoveryFile = entry.path();

			std::error_code timeEc;
			rec.savedAt = std::filesystem::last_write_time(entry.path(), timeEc);
			if (timeEc)
			{
				continue;
			}

			// Only worth offering if it is ahead of the saved scene. A recovery copy older
			// than the scene means the user saved after it was written, so it holds nothing
			// the file does not already have - offering it would invite overwriting good
			// work with stale work.
			const auto sceneTime = std::filesystem::last_write_time(SceneFileFor(project, rec.sceneName), timeEc);
			if (!timeEc)
			{
				if (rec.savedAt <= sceneTime)
				{
					continue;
				}
				rec.secondsAheadOfScene = std::chrono::duration_cast<std::chrono::seconds>(rec.savedAt - sceneTime).count();
			}
			found.push_back(std::move(rec));
		}

		std::sort(found.begin(), found.end(), [](const RecoveredScene& a, const RecoveredScene& b) { return a.savedAt > b.savedAt; });
		return found;
	}

	bool AutosaveService::Restore(const app::EditorProjectContext& project, const std::string& sceneName, std::string& error)
	{
		const std::filesystem::path source = RecoveryDirectory(project) / (sceneName + std::string(kRecoverySuffix));
		if (!io::file_util::Exists(source))
		{
			error = "no recovery copy for scene '" + sceneName + "'";
			return false;
		}

		const auto text = io::file_util::ReadText(source);
		if (!text)
		{
			error = "could not read the recovery copy: " + text.error().message;
			return false;
		}
		// Parse BEFORE overwriting anything. Promoting a copy that does not parse would
		// destroy a scene that was merely out of date - the exact trade this service exists
		// to prevent, and the one property of it that must never regress.
		if (!app::scene::ParseToml(*text))
		{
			error = "the recovery copy for '" + sceneName + "' is not a readable scene; leaving the saved scene alone";
			return false;
		}

		if (auto written = io::file_util::WriteText(SceneFileFor(project, sceneName), *text); !written)
		{
			error = "could not write the scene file: " + written.error().message;
			return false;
		}
		AE_INFO(LogCategory::App, "Restored '{}' from its recovery copy.", sceneName);
		return true;
	}

	void AutosaveService::Discard(const app::EditorProjectContext& project, const std::string& sceneName)
	{
		std::error_code ec;
		std::filesystem::remove(RecoveryDirectory(project) / (sceneName + std::string(kRecoverySuffix)), ec);
	}
} // namespace aether::editor
