#include "editor/ShaderCompiler.hpp"

#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "io/FileUtil.hpp"
#include "io/Process.hpp"
#include "materialgraph/MaterialGraph.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
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

		bool IsOutputStale(const fs::path& source, const fs::path& output, const std::optional<fs::file_time_type>& newestSourceMTime)
		{
			const auto outputTime = LatestWriteTime(output);
			if (!outputTime)
			{
				return true;
			}
			const auto sourceTime = LatestWriteTime(source);
			if (!sourceTime || *sourceTime > *outputTime)
			{
				return true;
			}
			return newestSourceMTime.has_value() && *newestSourceMTime > *outputTime;
		}

		// header edit, but never under-compiles (stale binary shipped).
		bool IsShaderHeaderExtension(const fs::path& extension)
		{
			return extension == ".slang" || extension == ".slangh" || extension == ".hlsl" || extension == ".h";
		}

		std::optional<fs::file_time_type> NewestShaderSourceMTime(const fs::path& sourceDir)
		{
			std::optional<fs::file_time_type> newest;
			std::error_code ec;
			for (const auto& entry: fs::directory_iterator(sourceDir, ec))
			{
				if (ec)
				{
					break;
				}
				std::error_code fileEc;
				if (!entry.is_regular_file(fileEc) || fileEc || !IsShaderHeaderExtension(entry.path().extension()))
				{
					continue;
				}
				if (const auto mtime = LatestWriteTime(entry.path()); mtime && (!newest || *mtime > *newest))
				{
					newest = mtime;
				}
			}
			return newest;
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

		std::string Quoted(const fs::path& path)
		{
			return "\"" + path.string() + "\"";
		}
	} // namespace

	namespace
	{
		// The graph a material carries, or the one still sitting in its pre-single-file
		// sidecar. Returns nullopt for a material with no graph at all (a plain material, or
		// a hand-written shader that merely shares a name).
		std::optional<MaterialGraph> GraphForMaterial(const fs::path& materialFile)
		{
			const auto text = io::file_util::ReadText(materialFile);
			if (!text)
			{
				return std::nullopt;
			}
			if (auto graph = ParseMaterialGraph(*text); graph && !graph->nodes.empty())
			{
				return graph;
			}
			const fs::path sidecar = materialFile.parent_path() / (materialFile.stem().stem().generic_string() + ".materialgraph.toml");
			std::error_code sidecarEc;
			if (!fs::exists(sidecar, sidecarEc))
			{
				return std::nullopt;
			}
			const auto legacy = io::file_util::ReadText(sidecar);
			if (!legacy)
			{
				return std::nullopt;
			}
			auto graph = ParseMaterialGraph(MigrateLegacyGraphText(*legacy));
			if (graph && graph->nodes.empty())
			{
				return std::nullopt;
			}
			return graph;
		}

		// Rewrite every graph-derived shader whose generated text no longer matches its graph.
		// Written only when the text actually differs, so an up-to-date project does not churn
		// mtimes and force a full recompile on every open.
		void RegenerateGraphShaders(const fs::path& projectRoot, const fs::path& shaderDir)
		{
			const fs::path materialsDir = projectRoot / "assets" / "materials";
			std::error_code ec;
			if (!fs::is_directory(materialsDir, ec))
			{
				return;
			}
			for (const auto& entry: fs::recursive_directory_iterator(materialsDir, ec))
			{
				if (ec)
				{
					break;
				}
				if (!entry.is_regular_file(ec) || !entry.path().generic_string().ends_with(".material.toml"))
				{
					continue;
				}
				const std::optional<MaterialGraph> graph = GraphForMaterial(entry.path());
				if (!graph)
				{
					continue;
				}
				std::string error;
				const std::string shader = GenerateMaterialShader(*graph, error);
				if (shader.empty())
				{
					AE_WARN(LogCategory::App, "Material graph '{}' could not generate a shader: {}", entry.path().filename().generic_string(), error);
					continue;
				}
				const fs::path slangPath = shaderDir / (entry.path().stem().stem().generic_string() + ".slang");
				if (const auto existing = io::file_util::ReadText(slangPath); existing && *existing == shader)
				{
					continue;
				}
				if (io::file_util::WriteText(slangPath, shader))
				{
					AE_INFO(LogCategory::App, "Regenerated '{}' from its material graph.", slangPath.filename().generic_string());
				}
			}
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
		const auto newestSourceMTime = NewestShaderSourceMTime(slangFile.parent_path());
		if (!IsOutputStale(slangFile, outFile, newestSourceMTime))
		{
			return true;
		}

		if (auto dirResult = io::file_util::CreateDirectories(outDir); !dirResult)
		{
			error = "Could not create shader output folder: " + dirResult.error().message;
			return false;
		}

		// Compile to a temp file and only promote it over the real .spv on success. A failed,
		// crashed, or interrupted slangc then never leaves the shaders:// overlay serving a
		// partial or truncated binary - the last-good .spv survives untouched, so a broken edit
		// keeps rendering the previous shader instead of feeding garbage SPIR-V to Vulkan.
		// (Format is fixed by "-target spirv" in AETHER_SLANG_ARGS, so the .tmp extension is safe.)
		const fs::path tmpFile = outDir / (slangFile.stem().string() + ".spv.tmp");
		std::error_code tmpEc;
		fs::remove(tmpFile, tmpEc); // clear any leftover temp from a previously interrupted run

		// Let project shaders #include the engine's shared shader headers (DevicePointer,
		// FrameConstants, ...) by adding the engine shader dir to slangc's include search path.
		std::string includeArg;
#ifdef AETHER_SHADER_INCLUDE_DIR
		includeArg = " -I " + Quoted(AETHER_SHADER_INCLUDE_DIR);
#endif
		const std::string command = Quoted(AETHER_SLANGC_EXE) + " " AETHER_SLANG_ARGS + includeArg + " -o " + Quoted(tmpFile) + " " + Quoted(slangFile);
		const fs::path logFile = outDir / (slangFile.stem().string() + ".slangc.log");
		const int rc = io::RunProcessToLog(command, logFile);
		if (rc != 0)
		{
			fs::remove(tmpFile, tmpEc); // discard partial output; keep the last-good outFile intact
			std::string message = "slangc failed (exit " + std::to_string(rc) + ") on " + slangFile.filename().string() + ".";
			if (const std::string excerpt = ReadLogExcerpt(logFile); !excerpt.empty())
			{
				message += " " + excerpt;
			}
			error = std::move(message);
			return false;
		}

		std::error_code renameEc;
		fs::rename(tmpFile, outFile, renameEc);
		if (renameEc)
		{
			fs::remove(tmpFile, tmpEc);
			error = "Could not replace shader output '" + outFile.filename().string() + "': " + renameEc.message();
			return false;
		}

		AE_INFO(LogCategory::App, "Compiled shader '{}' -> '{}'", slangFile.generic_string(), outFile.generic_string());
		return true;
#else
		(void) slangFile;
		(void) outDir;
		(void) error;
		return true;
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

		// A graph-generated .slang is derived data, not a source file: regenerate it from the
		// graph before compiling. Without this an engine-side change to a shared header (a new
		// parameter on a helper every generated shader calls) silently rots every shader
		// generated before it, and the project reports the same compile errors on every open
		// with no way out but re-saving each material by hand.
		RegenerateGraphShaders(projectRoot, sourceDir);

		const auto newestSourceMTime = NewestShaderSourceMTime(sourceDir);

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
			const bool wasStale = IsOutputStale(entry.path(), outFile, newestSourceMTime);

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
} // namespace aether::editor
