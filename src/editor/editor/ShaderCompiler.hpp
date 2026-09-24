#pragma once

#include <filesystem>
#include <string>

namespace aether::editor
{
	struct ShaderCompileResult
	{
		bool ok = false;
		std::string message;
		int compiled = 0;
		int failed = 0;
		// Shaders whose .spv was already newer than every source and header, so slangc was
		// never invoked for them.
		int upToDate = 0;
	};

	[[nodiscard]] bool CanCompileShaders();

	[[nodiscard]] std::filesystem::path ProjectShaderIntermediateDir(const std::filesystem::path& projectRoot);

	// Compiles one .slang to <outDir>/<stem>.spv. sourceDir (optional) names the shader
	// source root the file lives under: when given, the .spv mirrors the file's subpath
	// under it (assets/shaders/effects/Foo.slang -> effects/Foo.spv) and dependency
	// staleness is scanned from the whole tree, exactly as CompileProject does.
	bool CompileOne(const std::filesystem::path& slangFile, const std::filesystem::path& outDir, std::string& error, const std::filesystem::path& sourceDir = {});

	[[nodiscard]] ShaderCompileResult CompileProject(const std::filesystem::path& projectRoot);
} // namespace aether::editor
