#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
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

		// Parsed-prefab cache: a game may instantiate the same prefab thousands of
		// times (bullets, pickups, enemies); without this each spawn re-read the
		// file and re-ran the TOML parser. Keyed by prefab name; refreshed on save
		// and cleared when the active project's prefab directory changes.
		std::mutex g_prefabCacheMutex;
		std::unordered_map<std::string, SceneDescription> g_prefabCache;

		// Serializes concurrent scene/prefab file writes. SaveSceneFile/SavePrefabFile
		// may be driven from the BackgroundSceneWriter thread as well as the main
		// thread (MCP scene.save, launcher hand-off); this keeps two writers from
		// interleaving into the same file.
		std::mutex g_sceneWriteMutex;

		void InvalidatePrefabCache()
		{
			const std::lock_guard<std::mutex> lock(g_prefabCacheMutex);
			g_prefabCache.clear();
		}

		constexpr std::string_view kProjectScenesVfsDir = "scenes";
		constexpr std::string_view kProjectPrefabsVfsDir = "assets/prefabs";
		constexpr std::string_view kSceneSuffix = ".scene.toml";
		constexpr std::string_view kPrefabSuffix = ".prefab.toml";
		// Cooked binary siblings (see WriteSceneBinary): loaded in preference to the
		// TOML source at runtime for a fast, tokenizer-free parse.
		constexpr std::string_view kSceneBinSuffix = ".scene.bin";
		constexpr std::string_view kPrefabBinSuffix = ".prefab.bin";

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

		// Load the cooked binary sibling (project VFS first, then disk), decode it,
		// and return the scene - or nullopt if there's no valid binary (caller then
		// falls back to the TOML source).
		std::optional<SceneDescription> TryReadBinary(std::string_view vfsDir, const std::string& name, std::string_view binSuffix, const std::filesystem::path& diskDir)
		{
			if (io::FileSystem::IsInitialized() && io::FileSystem::IsMounted("project"))
			{
				if (auto bytes = io::FileSystem::ReadFile(ProjectVirtualPath(vfsDir, name, binSuffix)); bytes)
				{
					if (auto scene = ReadSceneBinary(*bytes))
					{
						return scene;
					}
				}
			}
			if (auto bytes = io::file_util::ReadBinary(diskDir / (name + std::string(binSuffix))); bytes)
			{
				if (auto scene = ReadSceneBinary(*bytes))
				{
					return scene;
				}
			}
			return std::nullopt;
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
		if (g_projectPrefabsDirectory != prefabsDir)
		{
			InvalidatePrefabCache();
		}
		g_projectScenesDirectory = std::move(scenesDir);
		g_projectPrefabsDirectory = std::move(prefabsDir);
	}

	void ClearProjectSceneDirectories()
	{
		g_projectScenesDirectory.clear();
		g_projectPrefabsDirectory.clear();
		InvalidatePrefabCache();
	}

	const std::vector<std::string>& GenericComponentTypeNames()
	{
		// Pure data-only components: no asset resolution, physics bodies, cross-entity
		// refs, or bespoke serialization - just reflected fields. Capture/Apply/codec
		// handle these generically, so a new one only needs its AE_COMPONENT
		// declaration plus an entry here.
		static const std::vector<std::string> kNames{
		        "Bob",
		        "Spin",
		        "Orbit",
		        "Material Pulse",
		        "Scale Pulse",
		        "Look At",
		        "Parallax",
		};
		return kNames;
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

	void AssignPrefabGuids(SceneDescription& prefab)
	{
		std::uint64_t maxGuid = 0;
		for (const EntityRecord& e: prefab.entities)
		{
			maxGuid = std::max(maxGuid, e.guid);
		}
		for (EntityRecord& e: prefab.entities)
		{
			if (e.guid == 0)
			{
				e.guid = ++maxGuid;
			}
		}
	}

	bool SavePrefabFile(const std::string& prefabName, const SceneDescription& prefabIn)
	{
		const std::filesystem::path dir{PrefabsDirectory()};
		if (!io::file_util::CreateDirectories(dir))
		{
			AE_WARN(LogCategory::App, "SavePrefabFile: cannot create directory '{}'", dir.string());
			return false;
		}
		const std::filesystem::path path = dir / (prefabName + ".prefab.toml");

		// Assign stable guids so instance overrides survive future prefab edits.
		SceneDescription prefab = prefabIn;
		AssignPrefabGuids(prefab);

		std::string tomlText;
		std::vector<std::byte> binary;
		SerializeScene(prefab, tomlText, binary);

		{
			const std::lock_guard<std::mutex> lock(g_sceneWriteMutex);
			if (!io::file_util::WriteText(path, tomlText))
			{
				AE_WARN(LogCategory::App, "SavePrefabFile: cannot write '{}'", path.string());
				return false;
			}
			// Cook the binary sibling (best-effort; the TOML is the source of truth).
			if (auto cooked = io::file_util::WriteBinary(dir / (prefabName + std::string(kPrefabBinSuffix)), binary); !cooked)
			{
				AE_WARN(LogCategory::App, "SavePrefabFile: cannot cook binary for '{}': {}", prefabName, cooked.error().message);
			}
		}
		// Refresh the cache so the next instantiation sees the saved edit without a re-read.
		{
			const std::lock_guard<std::mutex> lock(g_prefabCacheMutex);
			g_prefabCache[prefabName] = prefab;
		}
		AE_INFO(LogCategory::App, "Prefab saved: {} ({} entities)", path.string(), prefab.entities.size());
		return true;
	}

	std::optional<SceneDescription> ReadPrefabFile(const std::string& prefabName)
	{
		{
			const std::lock_guard<std::mutex> lock(g_prefabCacheMutex);
			if (const auto it = g_prefabCache.find(prefabName); it != g_prefabCache.end())
			{
				return it->second;
			}
		}

		std::optional<SceneDescription> parsed = TryReadBinary(kProjectPrefabsVfsDir, prefabName, kPrefabBinSuffix, std::filesystem::path{PrefabsDirectory()});
		if (parsed)
		{
			// cooked binary hit
		}
		else if (auto text = ReadProjectText(kProjectPrefabsVfsDir, prefabName, kPrefabSuffix))
		{
			parsed = ParseToml(*text);
		}
		else
		{
			const std::filesystem::path path = std::filesystem::path{PrefabsDirectory()} / (prefabName + ".prefab.toml");
			auto diskText = io::file_util::ReadText(path);
			if (!diskText)
			{
				AE_WARN(LogCategory::App, "ReadPrefabFile: cannot read '{}'", path.string());
				return std::nullopt;
			}
			parsed = ParseToml(*diskText);
		}

		if (parsed)
		{
			const std::lock_guard<std::mutex> lock(g_prefabCacheMutex);
			g_prefabCache[prefabName] = *parsed;
		}
		return parsed;
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

		std::string tomlText;
		std::vector<std::byte> binary;
		SerializeScene(scene, tomlText, binary);

		const std::lock_guard<std::mutex> lock(g_sceneWriteMutex);
		if (!io::file_util::WriteText(path, tomlText))
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot write '{}'", path.string());
			return false;
		}
		// Cook the binary sibling for fast runtime loads (best-effort; TOML is source).
		if (auto cooked = io::file_util::WriteBinary(dir / (sceneName + std::string(kSceneBinSuffix)), binary); !cooked)
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: cannot cook binary for '{}': {}", sceneName, cooked.error().message);
		}
		AE_INFO(LogCategory::App, "Scene saved: {} ({} entities)", path.string(), scene.entities.size());
		return true;
	}

	std::size_t CookProjectBinaries()
	{
		std::size_t cooked = 0;
		const auto cookOne = [&cooked](const std::filesystem::path& dir, const std::string& name, std::string_view tomlSuffix, std::string_view binSuffix)
		{
			const std::filesystem::path tomlPath = dir / (name + std::string(tomlSuffix));
			auto text = io::file_util::ReadText(tomlPath);
			if (!text)
			{
				return;
			}
			auto desc = ParseToml(*text);
			if (!desc)
			{
				return;
			}
			if (io::file_util::WriteBinary(dir / (name + std::string(binSuffix)), WriteSceneBinary(*desc)))
			{
				++cooked;
			}
		};

		const std::filesystem::path scenesDir{ScenesDirectory()};
		for (const std::string& name: ListSceneFiles())
		{
			cookOne(scenesDir, name, kSceneSuffix, kSceneBinSuffix);
		}
		const std::filesystem::path prefabsDir{PrefabsDirectory()};
		for (const std::string& name: ListPrefabFiles())
		{
			cookOne(prefabsDir, name, kPrefabSuffix, kPrefabBinSuffix);
		}
		AE_INFO(LogCategory::App, "Cooked {} scene/prefab binaries", cooked);
		return cooked;
	}

	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName)
	{
		// Prefer the cooked binary; fall back to the TOML source.
		if (auto binary = TryReadBinary(kProjectScenesVfsDir, sceneName, kSceneBinSuffix, std::filesystem::path{ScenesDirectory()}))
		{
			return binary;
		}

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
