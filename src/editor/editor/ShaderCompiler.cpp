#include "editor/ShaderCompiler.hpp"

#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <mutex>
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
			return extension == ".slangh" || extension == ".hlsl" || extension == ".h";
		}

		// Which .slang files are included by another shader, and so have to invalidate it when
		// they change. Treating EVERY .slang as a shared header instead - which is what
		// counting them all did - meant regenerating one material's shader marked all of its
		// siblings stale, and the editor recompiled every project shader on every open.
		std::unordered_set<std::string> IncludedShaderNames(const fs::path& sourceDir)
		{
			std::unordered_set<std::string> included;
			std::error_code ec;
			for (const auto& entry: fs::recursive_directory_iterator(sourceDir, ec))
			{
				if (ec)
				{
					break;
				}
				std::error_code fileEc;
				const fs::path extension = entry.path().extension();
				if (!entry.is_regular_file(fileEc) || fileEc || (extension != ".slang" && !IsShaderHeaderExtension(extension)))
				{
					continue;
				}
				const auto text = io::file_util::ReadText(entry.path());
				if (!text)
				{
					continue;
				}
				// Any quoted or angled name on an #include line. Cheap and deliberately loose:
				// over-listing a name only costs a recompile, missing one ships a stale binary.
				std::size_t at = text->find("#include");
				while (at != std::string::npos)
				{
					const std::size_t eol = text->find('\n', at);
					const std::string line = text->substr(at, eol == std::string::npos ? std::string::npos : eol - at);
					const std::size_t open = line.find_first_of("\"<");
					const std::size_t close = open == std::string::npos ? std::string::npos : line.find_first_of("\">", open + 1);
					if (open != std::string::npos && close != std::string::npos && close > open + 1)
					{
						included.insert(fs::path(line.substr(open + 1, close - open - 1)).filename().generic_string());
					}
					at = text->find("#include", at + 1);
				}
			}
			return included;
		}

		// Recursive: the engine's shader headers live in subdirectories, and a scan that only
		// looked at the top level would report the wrong answer for them.
		std::optional<fs::file_time_type> NewestShaderSourceMTime(const fs::path& sourceDir, const std::unordered_set<std::string>* includedShaders = nullptr)
		{
			std::optional<fs::file_time_type> newest;
			std::error_code ec;
			for (const auto& entry: fs::recursive_directory_iterator(sourceDir, ec))
			{
				if (ec)
				{
					break;
				}
				std::error_code fileEc;
				if (!entry.is_regular_file(fileEc) || fileEc)
				{
					continue;
				}
				const fs::path extension = entry.path().extension();
				const bool isIncludedShader = includedShaders != nullptr && extension == ".slang" && includedShaders->contains(entry.path().filename().generic_string());
				if (!IsShaderHeaderExtension(extension) && !isIncludedShader)
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
		// The shader a material file declares, e.g. "shaders://Rock.spv", or empty.
		std::string DeclaredShaderPath(const fs::path& materialFile)
		{
			const auto text = io::file_util::ReadText(materialFile);
			if (!text)
			{
				return {};
			}
			std::size_t at = text->find("shader");
			while (at != std::string::npos)
			{
				const bool atLineStart = at == 0 || (*text)[at - 1] == '\n' || (*text)[at - 1] == '\r';
				if (atLineStart)
				{
					const std::size_t eol = text->find('\n', at);
					const std::string line = text->substr(at, eol == std::string::npos ? std::string::npos : eol - at);
					const std::size_t open = line.find_first_of("'\"");
					const std::size_t close = open == std::string::npos ? std::string::npos : line.find(line[open], open + 1);
					if (open != std::string::npos && close != std::string::npos)
					{
						return line.substr(open + 1, close - open - 1);
					}
				}
				at = text->find("shader", at + 1);
			}
			return {};
		}

		// Two materials naming the same generated shader is silent corruption: the last one
		// regenerated wins and the other renders a graph that is not its own. Recompiling from
		// the Material window appears to fix it, right up until the next startup regenerates
		// and clobbers it again - which is exactly the kind of bug nobody can reproduce.
		//
		// Legacy ".toml" materials are checked too, precisely BECAUSE regeneration skips them:
		// one can silently lose its shader to a ".material.toml" of the same name.
		void WarnOnSharedShaders(const fs::path& materialsDir)
		{
			std::error_code ec;
			std::unordered_map<std::string, fs::path> claimedBy;
			for (const auto& entry: fs::recursive_directory_iterator(materialsDir, ec))
			{
				if (ec)
				{
					break;
				}
				if (!entry.is_regular_file(ec) || !entry.path().generic_string().ends_with(".toml"))
				{
					continue;
				}
				const std::string declared = DeclaredShaderPath(entry.path());
				if (declared.empty())
				{
					continue;
				}
				if (const auto [it, inserted] = claimedBy.try_emplace(declared, entry.path()); !inserted)
				{
					AE_ERROR(LogCategory::App,
					        "Materials '{}' and '{}' both use '{}'. Only one of them can generate it, so the other renders the wrong graph - rename or remove one.",
					        it->second.filename().generic_string(),
					        entry.path().filename().generic_string(),
					        declared);
				}
			}
		}

		void RegenerateGraphShaders(const fs::path& projectRoot, const fs::path& shaderDir)
		{
			const fs::path materialsDir = projectRoot / "assets" / "materials";
			std::error_code ec;
			if (!fs::is_directory(materialsDir, ec))
			{
				return;
			}
			WarnOnSharedShaders(materialsDir);
			// Which material generated which shader. Two materials writing the same shader is
			// silent corruption otherwise: the last one to run wins, the other renders someone
			// else's graph, and recompiling from the Material window appears to "fix" it until
			// the next startup regenerates and clobbers it again.
			std::unordered_map<std::string, fs::path> generatedBy;
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
				if (const auto [it, inserted] = generatedBy.try_emplace(slangPath.generic_string(), entry.path()); !inserted)
				{
					AE_ERROR(LogCategory::App,
					        "Materials '{}' and '{}' both generate '{}'. Only one of them can win, so the other renders the wrong graph. Rename or remove one.",
					        it->second.filename().generic_string(),
					        entry.path().filename().generic_string(),
					        slangPath.filename().generic_string());
					continue;
				}
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

	namespace
	{
		// Everything a shader in this directory depends on besides itself: the project's own
		// headers, any .slang another shader includes, and the engine's shader headers. Both
		// the single-shader and whole-project paths ask this same question, so they cannot
		// disagree about what counts as stale.
		std::optional<fs::file_time_type> NewestDependencyMTime(const fs::path& sourceDir)
		{
			const std::unordered_set<std::string> included = IncludedShaderNames(sourceDir);
			auto newest = NewestShaderSourceMTime(sourceDir, &included);
#ifdef AETHER_SHADER_INCLUDE_DIR
			// A change to an engine header has to invalidate every project shader built
			// against it, or the skip below ships a binary compiled against a header that no
			// longer exists in that form.
			if (const auto engineHeaders = NewestShaderSourceMTime(fs::path(AETHER_SHADER_INCLUDE_DIR)); engineHeaders && (!newest || *engineHeaders > *newest))
			{
				newest = engineHeaders;
			}
#endif
			return newest;
		}

		// Publish (worker thread), the Play recompile hook (main thread) and the Material
		// window's async compile all run slangc into the same intermediate directory. Two
		// concurrent runs would share <stem>.spv.tmp and <stem>.spv, and one run's cleanup
		// delete can promote the other's half-written binary. Recursive so CompileProject
		// can hold it across its loop of CompileOne calls.
		std::recursive_mutex& CompileMutex()
		{
			static std::recursive_mutex mutex;
			return mutex;
		}

		// Output path for a shader: its subpath under sourceDir with .spv, so the recursive
		// source scan and the outputs agree (assets/shaders/effects/Foo.slang ->
		// Intermediate/shaders/effects/Foo.spv) and two same-named shaders in different
		// folders cannot clobber each other. Falls back to a flat <stem>.spv for callers
		// that pass no sourceDir or a file outside it.
		fs::path ShaderOutputFile(const fs::path& slangFile, const fs::path& outDir, const fs::path& sourceDir)
		{
			fs::path name = slangFile.filename();
			if (!sourceDir.empty())
			{
				std::error_code ec;
				const fs::path relative = fs::relative(slangFile, sourceDir, ec);
				if (!ec && !relative.empty() && !relative.is_absolute() && *relative.begin() != fs::path(".."))
				{
					name = relative;
				}
			}
			return outDir / name.replace_extension(".spv");
		}
	} // namespace

	bool CompileOne(const fs::path& slangFile, const fs::path& outDir, std::string& error, const fs::path& sourceDir)
	{
#ifdef AETHER_SLANGC_EXE
		const std::lock_guard<std::recursive_mutex> compileLock(CompileMutex());
		const fs::path outFile = ShaderOutputFile(slangFile, outDir, sourceDir);
		// With a sourceDir, staleness is scanned from the whole source tree (a shader can
		// include a header or shader from any subdirectory), not just the file's own folder.
		const auto newestSourceMTime = NewestDependencyMTime(sourceDir.empty() ? slangFile.parent_path() : sourceDir);
		if (!IsOutputStale(slangFile, outFile, newestSourceMTime))
		{
			return true;
		}

		if (auto dirResult = io::file_util::CreateDirectories(outFile.parent_path()); !dirResult)
		{
			error = "Could not create shader output folder: " + dirResult.error().message;
			return false;
		}

		// Compile to a temp file and only promote it over the real .spv on success. A failed,
		// crashed, or interrupted slangc then never leaves the shaders:// overlay serving a
		// partial or truncated binary - the last-good .spv survives untouched, so a broken edit
		// keeps rendering the previous shader instead of feeding garbage SPIR-V to Vulkan.
		// (Format is fixed by "-target spirv" in AETHER_SLANG_ARGS, so the .tmp extension is safe.)
		fs::path tmpFile = outFile;
		tmpFile += ".tmp";
		std::error_code tmpEc;
		fs::remove(tmpFile, tmpEc); // clear any leftover temp from a previously interrupted run

		// Let project shaders #include the engine's shared shader headers (DevicePointer,
		// FrameConstants, ...) by adding the engine shader dir to slangc's include search path.
		std::string includeArg;
#ifdef AETHER_SHADER_INCLUDE_DIR
		includeArg = " -I " + Quoted(AETHER_SHADER_INCLUDE_DIR);
#endif
		const std::string command = Quoted(AETHER_SLANGC_EXE) + " " AETHER_SLANG_ARGS + includeArg + " -o " + Quoted(tmpFile) + " " + Quoted(slangFile);
		fs::path logFile = outFile;
		logFile.replace_extension(".slangc.log");
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
		(void) sourceDir;
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

		// Serialize against the publish worker and the Material window's async compile (see
		// CompileMutex): all of them run slangc over this same intermediate directory.
		const std::lock_guard<std::recursive_mutex> compileLock(CompileMutex());

		// A graph-generated .slang is derived data, not a source file: regenerate it from the
		// graph before compiling. Without this an engine-side change to a shared header (a new
		// parameter on a helper every generated shader calls) silently rots every shader
		// generated before it, and the project reports the same compile errors on every open
		// with no way out but re-saving each material by hand.
		RegenerateGraphShaders(projectRoot, sourceDir);

		const auto newestSourceMTime = NewestDependencyMTime(sourceDir);

		// Recursive, matching the dependency scans above: IncludedShaderNames and
		// NewestShaderSourceMTime already treat a .slang in any subdirectory as an
		// includable dependency, so the compile loop must reach it too - otherwise its
		// staleness is accounted against shaders that can never be rebuilt, and it neither
		// produces an .spv nor packs one. Outputs mirror the source subpath (see
		// ShaderOutputFile); PakWriter::AddDirectoryAs packs the tree recursively, and the
		// shaders:// overlay mounts the intermediate root, so subpaths resolve end to end.
		for (const auto& entry: fs::recursive_directory_iterator(sourceDir, ec))
		{
			if (ec)
			{
				break;
			}
			if (!entry.is_regular_file(ec) || entry.path().extension().string() != ".slang")
			{
				continue;
			}

			const fs::path outFile = ShaderOutputFile(entry.path(), outDir, sourceDir);
			// Staleness was already being computed here and used only to count: slangc ran on
			// every shader on every open and every play, which was most of the editor's
			// startup time for a project whose shaders had not changed at all.
			if (!IsOutputStale(entry.path(), outFile, newestSourceMTime))
			{
				++result.upToDate;
				continue;
			}

			std::string compileError;
			if (!CompileOne(entry.path(), outDir, compileError, sourceDir))
			{
				++result.failed;
				std::error_code relEc;
				const fs::path label = fs::relative(entry.path(), sourceDir, relEc);
				failures.push_back((relEc ? entry.path().filename() : label).generic_string() + ": " + compileError);
				continue;
			}
			++result.compiled;
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
			result.message = "Compiled " + std::to_string(result.compiled) + " shader(s), " + std::to_string(result.upToDate) + " up to date, " + std::to_string(result.failed) + " failed.";
		}

		AE_INFO(LogCategory::App, "Project shader compile ({}): {} compiled, {} up to date, {} failed.", projectRoot.generic_string(), result.compiled, result.upToDate, result.failed);
		return result;
	}
} // namespace aether::editor
