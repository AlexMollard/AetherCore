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

#ifdef AETHER_RUST_SDK_MANIFEST
		constexpr std::string_view kRustSdkDevHint = AETHER_RUST_SDK_MANIFEST;
#else
		constexpr std::string_view kRustSdkDevHint = {};
#endif
#ifdef AETHER_RUST_GAMEPLAY_SDK_MANIFEST
		constexpr std::string_view kRustGameplaySdk = AETHER_RUST_GAMEPLAY_SDK_MANIFEST;
#else
		constexpr std::string_view kRustGameplaySdk = {};
#endif
#ifdef AETHER_RUST_BUILD_PROJECT
		constexpr std::string_view kRustBuildProject = AETHER_RUST_BUILD_PROJECT;
#else
		constexpr std::string_view kRustBuildProject = {};
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

	std::filesystem::path EngineRustSdkManifest()
	{
		return io::PlatformPaths::ResolveBundlePath("AETHERCORE_RUST_SDK", kRustSdkDevHint, "data/sdk/rust/aethercore-abi/Cargo.toml");
	}
	std::filesystem::path EngineRustGameplaySdkManifest()
	{
		return io::PlatformPaths::ResolveBundlePath("AETHERCORE_RUST_GAMEPLAY_SDK", kRustGameplaySdk, "data/sdk/rust/aethercore/Cargo.toml");
	}
	std::filesystem::path EngineRustBuildProjectDir()
	{
		return io::PlatformPaths::ResolveBundlePath("AETHERCORE_RUST_BUILD", kRustBuildProject, "data/sdk/rust/build");
	}
} // namespace aether::app
