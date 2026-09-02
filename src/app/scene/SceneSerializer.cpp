#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include <entt/entt.hpp>

#include "EngineContentPaths.hpp"
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
			// Empty means no project has scoped a directory; joining onto it would resolve
			// relative to the process CWD and read whatever happened to be sitting there.
			if (diskDir.empty())
			{
				return std::nullopt;
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

		// The cooked .bin is a derived cache of the .toml source. When the source is
		// edited WITHOUT going through SaveSceneFile - a git checkout / merge / pull
		// (the .bin is gitignored, so it lags the pulled .toml), a hand-edit, or any
		// tool that rewrites only the .toml - the stale .bin silently wins at load and
		// the edits vanish at runtime (e.g. a GoalFlag.NextScene that still points at
		// the old value). Returns true when a loose .toml source exists on disk and is
		// newer than its cooked .bin sibling, so the caller reads from source instead.
		// A pak-only project has no loose .toml sibling (cooked atomically at publish),
		// so both stat calls fail and this returns false - the cooked binary is trusted.
		bool CookedBinaryIsStale(const std::filesystem::path& diskDir, const std::string& name,
		        std::string_view binSuffix, std::string_view tomlSuffix)
		{
			std::error_code ec;
			const auto binTime = std::filesystem::last_write_time(diskDir / (name + std::string(binSuffix)), ec);
			if (ec)
			{
				return false; // no loose cooked bin to compare (VFS/pak path) - trust whatever TryReadBinary finds
			}
			const auto tomlTime = std::filesystem::last_write_time(diskDir / (name + std::string(tomlSuffix)), ec);
			if (ec)
			{
				return false; // no loose source sibling - nothing newer to prefer
			}
			return tomlTime > binTime;
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

	std::string ScenesDirectory()
	{
		if (!g_projectScenesDirectory.empty())
		{
			return g_projectScenesDirectory.string();
		}
		if (const std::filesystem::path templates = EngineSceneTemplatesDir(); !templates.empty())
		{
			return templates.string();
		}
		return EngineSettingsIO::ResolvePath("scenes").string();
	}

	std::string PrefabsDirectory()
	{
		if (!g_projectPrefabsDirectory.empty())
		{
			return g_projectPrefabsDirectory.string();
		}
		if (const std::filesystem::path templates = EnginePrefabTemplatesDir(); !templates.empty())
		{
			return templates.string();
		}
		return EngineSettingsIO::ResolvePath("prefabs").string();
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

	std::uint64_t GenerateSceneNodeId()
	{
		static std::mt19937_64 rng{std::random_device{}()};
		std::uint64_t id = 0;
		while (id == 0) // 0 is the "unassigned" sentinel
		{
			id = rng();
		}
		return id;
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
		// Prefabs are header-less fragments (no [scene] block); a header would stamp
		// them kind='3d' with 3D features.
		SerializeScene(prefab, tomlText, binary, /*includeSceneHeader=*/false);

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

		// Only a project-scoped directory, for the same reason as ReadSceneFile: PrefabsDirectory()
		// substitutes the engine's shipped folder when nothing has scoped one, and a shipped runtime
		// never scopes one - so a file sitting in the engine's resources would answer a lookup for
		// the project's own prefab of that name, on the machine that built the game and nowhere else.
		// That folder does not exist today, which is the only reason this was latent rather than the
		// same bug scenes had.
		const std::filesystem::path prefabsDir = g_projectPrefabsDirectory;
		// Same freshness rule as scenes: a stale cooked .bin (e.g. after a git pull of
		// an edited .prefab.toml) must not shadow the newer source.
		std::optional<SceneDescription> parsed;
		if (prefabsDir.empty() || !CookedBinaryIsStale(prefabsDir, prefabName, kPrefabBinSuffix, kPrefabSuffix))
		{
			parsed = TryReadBinary(kProjectPrefabsVfsDir, prefabName, kPrefabBinSuffix, prefabsDir);
		}
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
			const std::filesystem::path path = prefabsDir.empty() ? std::filesystem::path{} : prefabsDir / (prefabName + ".prefab.toml");
			auto diskText = path.empty() ? decltype(io::file_util::ReadText(path)){} : io::file_util::ReadText(path);
			if (!diskText)
			{
				AE_WARN(LogCategory::App, "ReadPrefabFile: cannot read prefab '{}' from project:// {}", prefabName,
				        prefabsDir.empty() ? std::string{"(no project prefabs directory scoped)"} : prefabsDir.string());
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

	bool IsValidSceneName(std::string_view sceneName)
	{
		if (sceneName.empty())
		{
			return false;
		}
		// A name is pasted into a filename, so anything the filesystem treats specially has to
		// go. A colon was the one that mattered: "bad:name" wrote the scene into an NTFS
		// alternate data stream, left a 0-byte file called "bad", and reported success - the
		// scene simply was not where the user was told it was.
		constexpr std::string_view kForbidden = "<>:\"/\\|?*";
		for (const char c: sceneName)
		{
			if (kForbidden.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
			{
				return false;
			}
		}
		// "." and ".." name a directory, not a scene.
		return sceneName.find_first_not_of('.') != std::string_view::npos;
	}

	bool SaveSceneFile(const std::string& sceneName, const SceneDescription& scene)
	{
		if (!IsValidSceneName(sceneName))
		{
			AE_WARN(LogCategory::App, "SaveSceneFile: '{}' is not a usable scene name", sceneName);
			return false;
		}
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

		// A cook writes .bin siblings, so it must only ever run inside a project. Without this
		// guard ScenesDirectory() hands back the engine's shipped-template folder when no
		// project is scoped, and the cook drops project scenes into the engine's resources -
		// where they then shadow the real thing on every later load.
		if (g_projectScenesDirectory.empty())
		{
			AE_WARN(LogCategory::App, "CookProjectBinaries: no project scenes directory scoped; skipping (refusing to cook into the engine's resources).");
			return 0;
		}

		const std::filesystem::path scenesDir = g_projectScenesDirectory;
		for (const std::string& name: ListSceneFiles())
		{
			cookOne(scenesDir, name, kSceneSuffix, kSceneBinSuffix);
		}
		const std::filesystem::path prefabsDir = g_projectPrefabsDirectory;
		for (const std::string& name: ListPrefabFiles())
		{
			cookOne(prefabsDir, name, kPrefabSuffix, kPrefabBinSuffix);
		}
		AE_INFO(LogCategory::App, "Cooked {} scene/prefab binaries", cooked);
		return cooked;
	}

	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName)
	{
		// ONLY a directory a project has scoped may answer a scene lookup from disk.
		// ScenesDirectory() substitutes the engine's shipped-template folder when nothing has,
		// which is correct for the read-only fallback at the bottom of this function and wrong
		// here: a shipped runtime never scopes one, so that substitution let a file sitting in
		// the engine's resources shadow the project's own scene of the same name - silently, and
		// only on a machine where that folder exists, which is the machine that built the game.
		const std::filesystem::path scenesDir = g_projectScenesDirectory;

		// Prefer the cooked binary, but never a stale one (see CookedBinaryIsStale):
		// a stale .bin drops post-cook edits at runtime.
		if (scenesDir.empty() || !CookedBinaryIsStale(scenesDir, sceneName, kSceneBinSuffix, kSceneSuffix))
		{
			if (auto binary = TryReadBinary(kProjectScenesVfsDir, sceneName, kSceneBinSuffix, scenesDir))
			{
				return binary;
			}
		}
		else
		{
			AE_INFO(LogCategory::App,
			        "ReadSceneFile: cooked '{}{}' is older than its '{}' source; loading source and skipping the stale cook (re-save to refresh it)",
			        sceneName, kSceneBinSuffix, kSceneSuffix);
		}

		if (auto text = ReadProjectText(kProjectScenesVfsDir, sceneName, kSceneSuffix))
		{
			return ParseToml(*text);
		}

		if (!scenesDir.empty())
		{
			if (auto text = io::file_util::ReadText(scenesDir / (sceneName + std::string(kSceneSuffix))))
			{
				return ParseToml(*text);
			}
		}

		// Shipped templates ("default", "default2d"): older projects predate
		// some templates, so fall back to the engine's own copy.
		if (const std::filesystem::path templates = EngineSceneTemplatesDir(); !templates.empty())
		{
			if (auto text = io::file_util::ReadText(templates / (sceneName + ".scene.toml")))
			{
				return ParseToml(*text);
			}
		}

		AE_WARN(LogCategory::App, "ReadSceneFile: cannot read scene '{}' from project:// {}", sceneName, scenesDir.empty() ? std::string{"(no project scenes directory scoped)"} : scenesDir.string());
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
