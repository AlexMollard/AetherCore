#include "ProjectInspection.hpp"

#include <algorithm>
#include <fstream>
#include <set>

#include "EngineContentPaths.hpp"
#include "project/ProjectStartupScene.hpp"
#include "scene/SceneSerializer.hpp"

namespace aether::app::project
{
	namespace
	{
		using Json = nlohmann::json;

		std::string Utf8(const std::filesystem::path& path)
		{
			const auto value = path.generic_u8string();
			return {value.begin(), value.end()};
		}

		Json File(const std::filesystem::path& path)
		{
			std::error_code error;
			return {{"path", Utf8(path)}, {"exists", !path.empty() && std::filesystem::is_regular_file(path, error)}};
		}
	} // namespace

	ProjectInspection InspectProject(const EditorProjectContext& project, std::string_view sceneName, std::stop_token stop)
	{
		ProjectInspection result;
		auto& inventory = result.inventory;
		const auto issue = [&](std::string code, std::string message, const std::filesystem::path& path)
		{
			result.diagnostics.push_back({std::move(code), aether::scripting::AgentDiagnosticSeverity::Error, std::move(message), aether::scripting::AgentSourceLocation{Utf8(path)}});
		};
		inventory = {{"schemaVersion", 1},
		        {"scope", "authored-project"},
		        {"runtimeVerified", false},
		        {"compilerInvoked", false},
		        {"project", Utf8(project.root)},
		        {"name", project.name},
		        {"language", GameplayLanguageName(project.language)},
		        {"kind", project.kind == ProjectKind::Scene2D ? "2d" : "3d"},
		        {"paths", {{"descriptor", Utf8(project.projectFile)}, {"assets", Utf8(project.assetsDir)}, {"scenes", Utf8(project.scenesDir)}, {"prefabs", Utf8(project.prefabsDir)}, {"scripts", Utf8(project.scriptsDir)}}}};
		if (stop.stop_requested())
		{
			result.cancelled = true;
			return result;
		}
		const auto buildEntry = project.scriptsDir / (project.language == GameplayLanguage::Rust ? "Cargo.toml" : "AetherGame.csproj");
		inventory["gameplay"] = {{"buildEntry", File(buildEntry)}, {"sdk", File(project.language == GameplayLanguage::Rust ? EngineRustGameplaySdkManifest() : EngineManagedSdkProject())}, {"toolchainChecked", false}, {"scriptTypesReflected", false}};
		if (!inventory["gameplay"]["buildEntry"]["exists"].get<bool>())
		{
			issue("GAMEPLAY_INSPECT_BUILD_ENTRY", "Gameplay build entry is missing; restore it before building", buildEntry);
		}
		const auto startup = ReadProjectStartupScene(project.projectFile);
		inventory["startupScene"] = {{"name", startup}, {"exists", scene::IsValidAssetName(startup) && File(project.scenesDir / std::filesystem::u8path(startup + ".scene.toml"))["exists"].get<bool>()}};
		if (!inventory["startupScene"]["exists"].get<bool>())
		{
			issue("GAMEPLAY_INSPECT_STARTUP", "Set app.startupscene to an existing authored scene name", project.projectFile);
		}

		constexpr std::size_t kSceneLimit = 512;
		constexpr std::string_view kSuffix = ".scene.toml";
		std::set<std::string> scenes;
		std::size_t total = 0;
		std::error_code error;
		auto iterator = std::filesystem::recursive_directory_iterator(project.scenesDir, error);
		const auto end = std::filesystem::recursive_directory_iterator{};
		while (!error && iterator != end)
		{
			if (stop.stop_requested())
			{
				result.cancelled = true;
				return result;
			}
			if (iterator->is_regular_file(error))
			{
				const auto relative = Utf8(iterator->path().lexically_relative(project.scenesDir));
				if (relative.ends_with(kSuffix))
				{
					++total;
					scenes.insert(relative);
					if (scenes.size() > kSceneLimit)
					{
						scenes.erase(std::prev(scenes.end()));
					}
				}
			}
			if (!error)
			{
				iterator.increment(error);
			}
		}
		if (error)
		{
			issue("GAMEPLAY_INSPECT_SCENES", "Could not enumerate authored scenes: " + error.message(), project.scenesDir);
		}
		inventory["scenes"] = {{"entries", Json::array()}, {"total", total}, {"truncated", total > scenes.size()}, {"limit", kSceneLimit}};
		for (const auto& path: scenes)
		{
			const auto name = path.substr(0, path.size() - kSuffix.size());
			inventory["scenes"]["entries"].push_back({{"name", name}, {"path", Utf8(project.scenesDir / std::filesystem::u8path(path))}, {"loadableByName", scene::IsValidAssetName(name)}});
		}
		if (!sceneName.empty())
		{
			if (!scene::IsValidAssetName(sceneName))
			{
				issue("GAMEPLAY_INSPECT_SCENE_NAME", "Scene must be a bare scene name without an extension or directory", project.scenesDir);
				return result;
			}
			const auto path = project.scenesDir / std::filesystem::u8path(std::string(sceneName) + std::string(kSuffix));
			const auto bytes = std::filesystem::file_size(path, error);
			if (error || bytes > 1024 * 1024)
			{
				issue("GAMEPLAY_INSPECT_SCENE", "Scene must be readable authored TOML of at most 1 MiB", path);
				return result;
			}
			// Bound the read itself, including when a scene is edited after file_size.
			std::ifstream stream(path, std::ios::binary);
			std::string text(1024 * 1024 + 1, '\0');
			stream.read(text.data(), static_cast<std::streamsize>(text.size()));
			text.resize(static_cast<std::size_t>(stream.gcount()));
			if (stream.bad() || !stream.eof() || text.size() > 1024 * 1024)
			{
				issue("GAMEPLAY_INSPECT_SCENE", "Scene must be readable authored TOML of at most 1 MiB", path);
				return result;
			}
			const auto parsed = scene::ParseToml(text);
			if (!parsed)
			{
				issue("GAMEPLAY_INSPECT_SCENE", "Cannot parse authored scene TOML", path);
				return result;
			}
			constexpr std::size_t kEntityLimit = 200;
			auto& detail = inventory["scene"];
			detail = {{"name", sceneName},
			        {"path", Utf8(path)},
			        {"totalEntities", parsed->entities.size()},
			        {"truncated", parsed->entities.size() > kEntityLimit},
			        {"limit", kEntityLimit},
			        {"entities", Json::array()},
			        {"includes", parsed->includes},
			        {"prefabInstanceCount", parsed->prefabInstances.size()},
			        {"dependenciesExpanded", false}};
			for (std::size_t index = 0; index < std::min(parsed->entities.size(), kEntityLimit); ++index)
			{
				if (stop.stop_requested())
				{
					result.cancelled = true;
					return result;
				}
				const auto& entity = parsed->entities[index];
				Json entry = {{"index", index}, {"node", entity.nodeId}, {"name", entity.name}, {"parentIndex", entity.parentIndex}, {"disabled", entity.disabled}, {"scripts", Json::array()}};
				for (const auto& script: entity.scripts)
				{
					Json properties = Json::array();
					for (const auto& [name, value]: script.properties)
					{
						properties.push_back(name);
					}
					entry["scripts"].push_back({{"type", script.type}, {"authoredPropertyNames", properties}});
				}
				if (entity.hasTransform)
				{
					entry["position"] = {entity.position.x, entity.position.y, entity.position.z};
				}
				detail["entities"].push_back(std::move(entry));
			}
		}
		result.cancelled = stop.stop_requested();
		return result;
	}
} // namespace aether::app::project
