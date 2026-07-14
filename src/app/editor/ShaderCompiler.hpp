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
	};

	[[nodiscard]] bool CanCompileShaders();

	[[nodiscard]] std::filesystem::path ProjectShaderIntermediateDir(const std::filesystem::path& projectRoot);

	bool CompileOne(const std::filesystem::path& slangFile, const std::filesystem::path& outDir, std::string& error);

	[[nodiscard]] ShaderCompileResult CompileProject(const std::filesystem::path& projectRoot);
} // namespace aether::editor
