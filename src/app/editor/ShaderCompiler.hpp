#pragma once

#include <filesystem>
#include <string>

namespace aether::app
{
	// Result of compiling a project's Slang shaders. `compiled` counts shaders
	// that were actually invoked through slangc (stale outputs); shaders whose
	// .spv was already up to date are skipped and not counted as `compiled`.
	struct ShaderCompileResult
	{
		bool ok = false;
		std::string message;
		int compiled = 0;
		int failed = 0;
	};

	// True when this editor build was compiled with slangc wired in (a dev
	// checkout where CMake found the Slang toolchain - see
	// aethercore_enable_slang_shader_compilation in CMake/SlangShaders.cmake).
	// False in a shipped editor or on a machine without the Vulkan SDK, where
	// project shader compilation is a graceful no-op.
	[[nodiscard]] bool CanCompileShaders();

	// Convention shared with EditorProjectPublisher's managed-script build
	// (`<project>/Builds/Intermediate/managed`): compiled project shaders land
	// in `<projectRoot>/Builds/Intermediate/shaders`, which the dev shaders://
	// overlay layers on top of engine shaders (see
	// FileSystem::MountShaderOverlay).
	[[nodiscard]] std::filesystem::path ProjectShaderIntermediateDir(const std::filesystem::path& projectRoot);

	// Compiles a single .slang file to "<outDir>/<stem>.spv" via slangc, but
	// only if the output is missing, older than the source .slang file, or
	// older than the newest mtime among all shader-source files (.slang,
	// .slangh, .hlsl, .h) in the same directory - a conservative
	// header-dependency heuristic, since slangc reports no #include
	// dependency info here, so editing a shared .slangh recompiles every
	// .spv in the directory, not just the .slang that changed (mirrors
	// CSharpScriptingSubsystem::RebuildFromSource's own-mtime stale check,
	// extended for headers). The .slangc.log from a previous compile is
	// write-only diagnostics and is not consulted by the stale check.
	// Returns true when the output is up to date on return (either it
	// already was, or the compile just succeeded); on failure returns false
	// and fills `error` with a log excerpt. No-op success when this build
	// has no slangc wired in (CanCompileShaders() == false).
	bool CompileOne(const std::filesystem::path& slangFile, const std::filesystem::path& outDir, std::string& error);

	// Compiles every "<projectRoot>/assets/shaders/*.slang" into
	// ProjectShaderIntermediateDir(projectRoot), skipping up-to-date outputs
	// (see CompileOne). Used both when a project loads and from the manual
	// "Recompile Shaders" editor action (EditorProjectActions::recompileShaders).
	// A no-op success (ok=true, compiled=0, failed=0) when this build has no
	// slangc wired in, or the project has no assets/shaders directory.
	[[nodiscard]] ShaderCompileResult CompileProject(const std::filesystem::path& projectRoot);
} // namespace aether::app
