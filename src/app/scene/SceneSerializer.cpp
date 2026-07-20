#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <entt/entt.hpp>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/UiComponents.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"


namespace aether::app::scene
{
	namespace
	{
		std::filesystem::path g_projectScenesDirectory;
		std::filesystem::path g_projectPrefabsDirectory;

		constexpr std::string_view kProjectScenesVfsDir = "scenes";
		constexpr std::string_view kProjectPrefabsVfsDir = "assets/prefabs";
		constexpr std::string_view kSceneSuffix = ".scene.toml";
		constexpr std::string_view kPrefabSuffix = ".prefab.toml";

		std::string ProjectVirtualPath(std::string_view directory, const std::string& name, std::string_view suffix)
		{
			std::string path = "project://";
			path += directory;
			path += "/";
			path += name;
			path += suffix;
			return path;
		}

		std::optional<std::string> ReadProjectText(std::string_view directory, const std::string& name, std::string_view suffix)
		{
			if (!io::FileSystem::IsInitialized() || !io::FileSystem::IsMounted("project"))
			{
				return std::nullopt;
			}

			auto text = io::FileSystem::ReadFileText(ProjectVirtualPath(directory, name, suffix));
			if (!text)
			{
				return std::nullopt;
			}
			return std::move(*text);
		}

		std::vector<std::string> ListProjectFiles(std::string_view directory, std::string_view suffix)
		{
			std::vector<std::string> names;
			if (!io::FileSystem::IsInitialized() || !io::FileSystem::IsMounted("project"))
			{
				return names;
			}

			std::string pattern = "project://";
			pattern += directory;
			pattern += "/*";
			pattern += suffix;
			auto matches = io::FileSystem::Glob(pattern);
			if (!matches)
			{
				return names;
			}

			for (const std::string& match: *matches)
			{
				const std::string file = std::filesystem::path(match).filename().generic_string();
				if (file.size() > suffix.size() && file.ends_with(suffix))
				{
					names.push_back(file.substr(0, file.size() - suffix.size()));
				}
			}
			std::sort(names.begin(), names.end());
			return names;
		}
	} // namespace

	void SetProjectSceneDirectories(std::filesystem::path scenesDir, std::filesystem::path prefabsDir)
	{
		g_projectScenesDirectory = std::move(scenesDir);
		g_projectPrefabsDirectory = std::move(prefabsDir);
	}

	void ClearProjectSceneDirectories()
	{
		g_projectScenesDirectory.clear();
		g_projectPrefabsDirectory.clear();
	}

	std::string ScenesDirectory()
	{
		if (!g_projectScenesDirectory.empty())
		{
			return g_projectScenesDirectory.string();
		}
#ifdef AETHER_SCENES_SOURCE_DIR
		return AETHER_SCENES_SOURCE_DIR;
#else
		return EngineSettingsIO::ResolvePath("scenes").string();
#endif
	}

	std::string PrefabsDirectory()
	{
		if (!g_projectPrefabsDirectory.empty())
		{
			return g_projectPrefabsDirectory.string();
		}
#ifdef AETHER_PREFABS_SOURCE_DIR
		return AETHER_PREFABS_SOURCE_DIR;
#else
		return EngineSettingsIO::ResolvePath("prefabs").string();
#endif
	}

	bool SavePrefabFile(const std::string& prefabName, const SceneDescription& prefab)
	{
		const std::filesystem::path dir{PrefabsDirectory()};
		if (!io::file_util::CreateDirectories(dir))
		{
			AE_WARN(LogCategory::App, "SavePrefabFile: cannot create directory '{}'", dir.string());
			return false;
		}
		const std::filesystem::path path = dir / (prefabName + ".prefab.toml");

		if (!io::file_util::WriteText(path, WriteToml(prefab)))
		{
			AE_WARN(LogCategory::App, "SavePrefabFile: cannot write '{}'", path.string());
			return false;
		}
		AE_INFO(LogCategory::App, "Prefab saved: {} ({} entities)", path.string(), prefab.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadPrefabFile(const std::string& prefabName)
	{
		if (auto text = ReadProjectText(kProjectPrefabsVfsDir, prefabName, kPrefabSuffix))
		{
			return ParseToml(*text);
		}

		const std::filesystem::path path = std::filesystem::path{PrefabsDirectory()} / (prefabName + ".prefab.toml");
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			AE_WARN(LogCategory::App, "ReadPrefabFile: cannot read '{}'", path.string());
			return std::nullopt;
		}
		return ParseToml(*text);
	}

	std::vector<std::string> ListPrefabFiles()
	{
		std::vector<std::string> names = ListProjectFiles(kProjectPrefabsVfsDir, kPrefabSuffix);
		if (!names.empty())
		{
			return names;
		}

		const std::filesystem::path dir{PrefabsDirectory()};
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			const std::string file = entry.path().filename().string();
			constexpr std::string_view kSuffix = kPrefabSuffix;
			if (file.size() > kSuffix.size() && file.ends_with(kSuffix))
			{
				names.push_back(file.substr(0, file.size() - kSuffix.size()));
			}
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	bool SaveSceneFile(const std::string& sceneName, const SceneDescription& scene)
	{
		const std::filesystem::path dir{ScenesDirectory()};
		if (!io::file_util::CreateDirectories(dir))
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot create directory '{}'", dir.string());
			return false;
		}
		const std::filesystem::path path = dir / (sceneName + ".scene.toml");

		if (!io::file_util::WriteText(path, WriteToml(scene)))
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot write '{}'", path.string());
			return false;
		}
		AE_INFO(LogCategory::App, "Scene saved: {} ({} entities)", path.string(), scene.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName)
	{
		if (auto text = ReadProjectText(kProjectScenesVfsDir, sceneName, kSceneSuffix))
		{
			return ParseToml(*text);
		}

		const std::filesystem::path path = std::filesystem::path{ScenesDirectory()} / (sceneName + ".scene.toml");
		if (auto text = io::file_util::ReadText(path))
		{
			return ParseToml(*text);
		}

#ifdef AETHER_SCENES_SOURCE_DIR
		// Shipped templates ("default", "default2d"): older projects predate
		// some templates, so fall back to the engine's resources.
		const std::filesystem::path shipped = std::filesystem::path{AETHER_SCENES_SOURCE_DIR} / (sceneName + ".scene.toml");
		if (auto text = io::file_util::ReadText(shipped))
		{
			return ParseToml(*text);
		}
#endif

		AE_WARN(LogCategory::App, "ReadSceneFile: cannot read '{}'", path.string());
		return std::nullopt;
	}

	std::vector<std::string> ListSceneFiles()
	{
		std::vector<std::string> names = ListProjectFiles(kProjectScenesVfsDir, kSceneSuffix);
		if (!names.empty())
		{
			return names;
		}

		const std::filesystem::path dir{ScenesDirectory()};
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			const std::string file = entry.path().filename().string();
			constexpr std::string_view kSuffix = kSceneSuffix;
			if (file.size() > kSuffix.size() && file.ends_with(kSuffix))
			{
				names.push_back(file.substr(0, file.size() - kSuffix.size()));
			}
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	ApplySceneDeps MakeApplySceneDeps(ServiceContainer& services)
	{
		auto* sceneCtx = services.TryGet<scripting::SceneContext>();
		auto* assets = services.TryGet<AssetManager>();
		ApplySceneDeps deps{};
		deps.assets = assets;
		deps.primitives = services.TryGet<PrimitiveMeshes>();
		deps.effectManager = sceneCtx != nullptr ? sceneCtx->effects : nullptr;
		deps.effectParams = services.TryGet<EffectParamBuffer>();
		deps.pipelines = assets != nullptr ? &assets->GetPipelineCache() : nullptr;
		deps.sceneContext = sceneCtx;
		deps.physics = services.TryGet<PhysicsSystem>();
		deps.renderer = services.TryGet<Renderer>();
		deps.assetDatabase = services.TryGet<AssetDatabase>();
		if (const auto* bakeHook = services.TryGet<ModelBakeHook>(); bakeHook != nullptr)
		{
			deps.ensureModelBaked = bakeHook->ensureBaked;
		}
		return deps;
	}

} // namespace aether::app::scene
