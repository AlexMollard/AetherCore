#include "editor/ShaderCompiler.hpp"

#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "io/FileUtil.hpp"
#include "io/Process.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	namespace
	{
		namespace fs = std::filesystem;

		std::optional<fs::file_time_type> LatestWriteTime(const fs::path& path)
		{
			std::error_code ec;
			const auto time = fs::last_write_time(path, ec);
			if (ec)
			{
				return std::nullopt;
			}
			return time;
		}

		// True when `output` needs to be (re)built from `source`: missing, or
		// older than the source file. Mirrors the mtime-comparison stale check
		// CSharpScriptingSubsystem::RebuildFromSource uses for the C# build.
		bool IsOutputStale(const fs::path& source, const fs::path& output)
		{
			const auto outputTime = LatestWriteTime(output);
			if (!outputTime)
			{
				return true;
			}
			const auto sourceTime = LatestWriteTime(source);
			return !sourceTime || *sourceTime > *outputTime;
		}

		std::string ReadLogExcerpt(const fs::path& path)
		{
			auto text = io::file_util::ReadText(path);
			if (!text)
			{
				return {};
			}
			std::string result = std::move(*text);
			constexpr std::size_t kMaxExcerpt = 600;
			if (result.size() > kMaxExcerpt)
			{
				result.resize(kMaxExcerpt);
				result += "...";
			}
			return result;
		}

		// Quote a path for the shell command line, matching the
		// CSharpScriptingSubsystem::RebuildFromSource / EditorProjectPublisher
		// dotnet-invocation quoting convention (io::RunProcessToLog/Capture wrap
		// the whole command an extra time for cmd.exe's quote-stripping - see
		// io::WrapShellCommand).
		std::string Quoted(const fs::path& path)
		{
			return "\"" + path.string() + "\"";
		}
	} // namespace

	bool CanCompileShaders()
	{
#ifdef AETHER_SLANGC_EXE
		return true;
#else
		return false;
#endif
	}

	fs::path ProjectShaderIntermediateDir(const fs::path& projectRoot)
	{
		return projectRoot / "Builds" / "Intermediate" / "shaders";
	}

	bool CompileOne(const fs::path& slangFile, const fs::path& outDir, std::string& error)
	{
#ifdef AETHER_SLANGC_EXE
		const fs::path outFile = outDir / (slangFile.stem().string() + ".spv");
		if (!IsOutputStale(slangFile, outFile))
		{
			return true;
		}

		if (auto dirResult = io::file_util::CreateDirectories(outDir); !dirResult)
		{
			error = "Could not create shader output folder: " + dirResult.error().message;
			return false;
		}

		// Mirrors the CMake custom command in CMake/SlangShaders.cmake:
		// slangc <args> -o <out> <in>.
		const std::string command = Quoted(AETHER_SLANGC_EXE) + " " AETHER_SLANG_ARGS " -o " + Quoted(outFile) + " " + Quoted(slangFile);
		const fs::path logFile = outDir / (slangFile.stem().string() + ".slangc.log");
		const int rc = io::RunProcessToLog(command, logFile);
		if (rc != 0)
		{
			std::string message = "slangc failed (exit " + std::to_string(rc) + ") on " + slangFile.filename().string() + ".";
			if (const std::string excerpt = ReadLogExcerpt(logFile); !excerpt.empty())
			{
				message += " " + excerpt;
			}
			error = std::move(message);
			return false;
		}

		AE_INFO(LogCategory::App, "Compiled shader '{}' -> '{}'", slangFile.generic_string(), outFile.generic_string());
		return true;
#else
		(void) slangFile;
		(void) outDir;
		(void) error;
		return true; // no slangc wired into this build: graceful no-op
#endif
	}

	ShaderCompileResult CompileProject(const fs::path& projectRoot)
	{
		ShaderCompileResult result;
		result.ok = true;

		if (!CanCompileShaders())
		{
			result.message = "Shader compilation is unavailable in this build (no slangc found by CMake); skipping.";
			return result;
		}

		const fs::path sourceDir = projectRoot / "assets" / "shaders";
		std::error_code ec;
		if (!fs::is_directory(sourceDir, ec))
		{
			result.message = "No assets/shaders directory in this project; nothing to compile.";
			return result;
		}

		const fs::path outDir = ProjectShaderIntermediateDir(projectRoot);
		std::vector<std::string> failures;

		for (const auto& entry: fs::directory_iterator(sourceDir, ec))
		{
			if (ec)
			{
				break;
			}
			if (!entry.is_regular_file(ec) || entry.path().extension().string() != ".slang")
			{
				continue;
			}

			const fs::path outFile = outDir / (entry.path().stem().string() + ".spv");
			const bool wasStale = IsOutputStale(entry.path(), outFile);

			std::string compileError;
			if (!CompileOne(entry.path(), outDir, compileError))
			{
				++result.failed;
				failures.push_back(entry.path().filename().string() + ": " + compileError);
				continue;
			}
			if (wasStale)
			{
				++result.compiled;
			}
		}

		result.ok = result.failed == 0;
		if (!failures.empty())
		{
			result.message = std::to_string(result.failed) + " shader(s) failed to compile:\n";
			for (const std::string& failure: failures)
			{
				result.message += "  - " + failure + "\n";
			}
		}
		else
		{
			result.message = "Compiled " + std::to_string(result.compiled) + " shader(s), " + std::to_string(result.failed) + " failed.";
		}

		AE_INFO(LogCategory::App, "Project shader compile ({}): {} compiled, {} failed.", projectRoot.generic_string(), result.compiled, result.failed);
		return result;
	}
} // namespace aether::app
