#include "EngineContentPaths.hpp"

#include <string_view>

#include "io/PlatformPaths.hpp"

namespace aether::app
{
	namespace
	{
		// The build-time source paths, each present only on the targets CMake defines it
		// for. Absent is a normal state, not an error: a packaged build defines none of
		// them, and ResolveBundlePath simply moves on to the staged copy.
#ifdef AETHER_SCENES_SOURCE_DIR
		constexpr std::string_view kScenesDevHint = AETHER_SCENES_SOURCE_DIR;
#else
		constexpr std::string_view kScenesDevHint = {};
#endif
#ifdef AETHER_PREFABS_SOURCE_DIR
		constexpr std::string_view kPrefabsDevHint = AETHER_PREFABS_SOURCE_DIR;
#else
		constexpr std::string_view kPrefabsDevHint = {};
#endif
#ifdef AETHER_MANAGED_SDK_PROJECT
		constexpr std::string_view kSdkDevHint = AETHER_MANAGED_SDK_PROJECT;
#else
		constexpr std::string_view kSdkDevHint = {};
#endif
	} // namespace

	std::filesystem::path EngineSceneTemplatesDir()
	{
		return io::PlatformPaths::ResolveBundlePath("AETHERCORE_SCENE_TEMPLATES", kScenesDevHint, "data/templates/scenes");
	}

	std::filesystem::path EnginePrefabTemplatesDir()
	{
		return io::PlatformPaths::ResolveBundlePath("AETHERCORE_PREFAB_TEMPLATES", kPrefabsDevHint, "data/templates/prefabs");
	}

	std::filesystem::path EngineManagedSdkProject()
	{
		return io::PlatformPaths::ResolveBundlePath("AETHERCORE_SDK", kSdkDevHint, "data/sdk/managed/AetherCore/AetherCore.csproj");
	}
} // namespace aether::app
