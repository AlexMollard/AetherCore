// The recovery half of AutosaveService, kept in its own TU so it can be tested.
//
// Tick() needs a live editor - LayerContext, PlayState, the asset manager, the IO
// executor. These three do not: they are pure filesystem rules over a project layout,
// and they are the half where a mistake destroys work rather than merely failing to
// save it. Same split, and same reason, as RuntimeProjectSettings.
#include "editor/AutosaveService.hpp"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

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

		// Two documents describing the same scene are not the same text. A capture walks the
		// ECS, so its entity order is not the file's; an entity with no `node` in the file is
		// given one on load and it comes back on the next write; and a whole number reads as
		// `86` from a hand-edited file and `86.0` from a capture.
		//
		// Parse -> normalise -> re-serialise removes all three: parsing turns both number
		// forms into the same float, identity ids are cleared, and entity order is made
		// deterministic. parentIndex is POSITIONAL, so the sort has to carry it through the
		// permutation or the same parenting would serialise differently in the two documents.
		//
		// Returns empty when the text does not parse, which callers treat as "cannot tell" -
		// the safe answer for a recovery copy is to offer it.
		std::string CanonicalSceneText(const std::string_view tomlText)
		{
			std::optional<app::scene::SceneDescription> parsed = app::scene::ParseToml(tomlText);
			if (!parsed)
			{
				return {};
			}

			const std::size_t count = parsed->entities.size();
			std::vector<std::size_t> order(count);
			std::iota(order.begin(), order.end(), 0);
			// Name plus position separates everything that is actually distinct (fifteen
			// coins sit at fifteen places). Entities that still tie are interchangeable, so
			// which one wins does not change the text.
			const auto sortKey = [&parsed](const std::size_t index)
			{
				const app::scene::EntityRecord& record = parsed->entities[index];
				return std::tuple(std::string_view(record.name), record.position.x, record.position.y, record.position.z);
			};
			std::ranges::stable_sort(order, [&sortKey](const std::size_t a, const std::size_t b) { return sortKey(a) < sortKey(b); });

			std::vector<std::size_t> newIndexOf(count);
			for (std::size_t slot = 0; slot < count; ++slot)
			{
				newIndexOf[order[slot]] = slot;
			}

			std::vector<app::scene::EntityRecord> normalised;
			normalised.reserve(count);
			for (const std::size_t oldIndex: order)
			{
				app::scene::EntityRecord record = parsed->entities[oldIndex];
				// Identity, not content: a scene loaded from a file that predates node ids
				// gains them, which says nothing about whether the work differs.
				record.nodeId = 0;
				record.parentNodeId = 0;
				record.guid = 0;
				record.entityId = 0;
				if (record.parentIndex >= 0 && static_cast<std::size_t>(record.parentIndex) < count)
				{
					record.parentIndex = static_cast<int>(newIndexOf[static_cast<std::size_t>(record.parentIndex)]);
				}
				normalised.push_back(std::move(record));
			}
			parsed->entities = std::move(normalised);

			std::ranges::stable_sort(parsed->prefabInstances,
			        [](const app::scene::PrefabInstanceRecord& a, const app::scene::PrefabInstanceRecord& b)
			        { return std::tuple(std::string_view(a.name), std::string_view(a.prefabPath)) < std::tuple(std::string_view(b.name), std::string_view(b.prefabPath)); });

			return app::scene::WriteToml(*parsed, true);
		}
	} // namespace

	bool AutosaveService::HoldsNothingNew(const std::filesystem::path& recoveryFile, const std::filesystem::path& sceneFile)
	{
		const auto recoveryText = io::file_util::ReadText(recoveryFile);
		const auto sceneText = io::file_util::ReadText(sceneFile);
		if (!recoveryText || !sceneText)
		{
			return false; // cannot compare - offer it rather than drop it
		}
		const std::string canonicalRecovery = CanonicalSceneText(*recoveryText);
		if (canonicalRecovery.empty())
		{
			return false;
		}
		const std::string canonicalScene = CanonicalSceneText(*sceneText);
		if (canonicalScene.empty())
		{
			return false;
		}
		return canonicalRecovery == canonicalScene;
	}

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
			const std::filesystem::path sceneFile = SceneFileFor(project, rec.sceneName);
			const auto sceneTime = std::filesystem::last_write_time(sceneFile, timeEc);
			if (!timeEc)
			{
				if (rec.savedAt <= sceneTime)
				{
					continue;
				}
				rec.secondsAheadOfScene = std::chrono::duration_cast<std::chrono::seconds>(rec.savedAt - sceneTime).count();
			}

			// Newer is not the same as different. Autosave writes whenever the edit history
			// is dirty, and that stays dirty after an undo has put the scene back to what is
			// on disk - so a copy can be newer and hold nothing at all. Offering those is how
			// a recovery prompt teaches people to dismiss it without reading, which is the
			// one thing it cannot afford.
			if (HoldsNothingNew(entry.path(), sceneFile))
			{
				continue;
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
