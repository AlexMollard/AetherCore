#pragma once

#include <filesystem>

namespace aether::app
{
	// Where the content the ENGINE ships actually lives.
	//
	// Three of these used to be compile-time constants pointing straight into the source
	// tree - AETHER_SCENES_SOURCE_DIR and friends, baked in as absolute paths by CMake.
	// That is correct for whoever built the engine and wrong for everybody else: the
	// editor would launch on a machine that had only ever downloaded it, and then fail
	// the moment it tried to scaffold a project, because it was reaching for a directory
	// on the build machine's disk.
	//
	// Each of these resolves an environment override, then that build-time source path,
	// then the copy staged beside the executable (see StageAppBundle). A developer keeps
	// working against the tree they are editing; a packaged install finds its own copy.
	//
	// All of them return an EMPTY path when the content is genuinely missing, so callers
	// report an incomplete install rather than a confusing downstream error.

	// Scene templates a new project is seeded from ("default", "default2d").
	[[nodiscard]] std::filesystem::path EngineSceneTemplatesDir();

	// Prefab templates, resolved the same way.
	[[nodiscard]] std::filesystem::path EnginePrefabTemplatesDir();

	// The managed SDK's C# project, referenced by a generated game .csproj.
	[[nodiscard]] std::filesystem::path EngineManagedSdkProject();
} // namespace aether::app
