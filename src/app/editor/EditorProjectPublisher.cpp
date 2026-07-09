#include "editor/EditorProjectPublisher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "editor/EditorProjectContext.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

using namespace std::string_view_literals;

namespace aether::app
{
	namespace
	{
		constexpr std::string_view kProjectDirectory = ".project";
		constexpr std::string_view kProjectDescriptor = "aether.project";

		std::filesystem::path NormalizePath(std::filesystem::path path)
		{
			std::error_code ec;
			if (path.empty())
			{
				return {};
			}
			path = std::filesystem::absolute(path, ec);
			if (ec)
			{
				return path.lexically_normal();
			}
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
			return ec ? path.lexically_normal() : canonical;
		}

		std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		std::string LowerAscii(std::string value)
		{
			for (char& c: value)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return value;
		}

		bool PathStartsWith(std::filesystem::path path, std::filesystem::path parent)
		{
			path = NormalizePath(std::move(path));
			parent = NormalizePath(std::move(parent));
#ifdef _WIN32
			const std::string pathText = LowerAscii(path.generic_string());
			const std::string parentText = LowerAscii(parent.generic_string());
#else
			const std::string pathText = path.generic_string();
			const std::string parentText = parent.generic_string();
#endif
			return pathText == parentText || (pathText.starts_with(parentText) && pathText.size() > parentText.size() && (pathText[parentText.size()] == '/' || parentText.ends_with('/')));
		}

		std::string ShellQuotePath(const std::filesystem::path& path)
		{
			std::string text = path.string();
			std::string quoted = "\"";
			for (const char c: text)
			{
				if (c == '"')
				{
					quoted += "\\\"";
				}
				else
				{
					quoted += c;
				}
			}
			quoted += '"';
			return quoted;
		}

		std::string ReadLogExcerpt(const std::filesystem::path& path)
		{
			auto text = io::file_util::ReadText(path);
			if (!text)
			{
				return {};
			}

			std::string result = std::move(*text);
			constexpr std::size_t kMaxExcerpt = 420;
			if (result.size() > kMaxExcerpt)
			{
				result.resize(kMaxExcerpt);
				result += "...";
			}
			return result;
		}

		bool RunCommandToLog(const std::string& command, const std::filesystem::path& logPath, std::string& error)
		{
			if (auto dirResult = io::file_util::CreateDirectories(logPath.parent_path()); !dirResult)
			{
				error = "Could not create log directory: " + dirResult.error().message;
				return false;
			}
			const std::string wrapped = command + " > " + ShellQuotePath(logPath) + " 2>&1";
			const int exitCode = std::system(wrapped.c_str());
			if (exitCode != 0)
			{
				error = "Command failed with exit code " + std::to_string(exitCode) + ".";
				const std::string excerpt = ReadLogExcerpt(logPath);
				if (!excerpt.empty())
				{
					error += " " + excerpt;
				}
				return false;
			}
			return true;
		}

		std::string PublishPlatformDirectoryName()
		{
#ifdef _WIN32
			return "Windows";
#elif defined(__APPLE__)
			return "macOS";
#elif defined(__linux__)
			return "Linux";
#else
			return "Desktop";
#endif
		}

		std::string DefaultRuntimeExecutableName()
		{
#ifdef _WIN32
			return "AetherGame.exe";
#else
			return "AetherGame";
#endif
		}

		std::string EditorExecutableName()
		{
#ifdef _WIN32
			return "App.exe";
#else
			return "App";
#endif
		}

		bool CopyDirectoryRecursive(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(source, ec))
			{
				error = "Source folder does not exist: " + DisplayPath(source);
				return false;
			}
			if (auto dirResult = io::file_util::CreateDirectories(destination); !dirResult)
			{
				error = "Could not create output folder: " + dirResult.error().message;
				return false;
			}
			for (const auto& entry: std::filesystem::recursive_directory_iterator(source, ec))
			{
				if (ec)
				{
					error = "Could not read folder: " + ec.message();
					return false;
				}
				const std::filesystem::path rel = std::filesystem::relative(entry.path(), source, ec);
				if (ec)
				{
					error = "Could not resolve relative path: " + ec.message();
					return false;
				}
				const std::filesystem::path target = destination / rel;
				if (entry.is_directory(ec))
				{
					if (auto dirResult = io::file_util::CreateDirectories(target); !dirResult)
					{
						error = "Could not create output folder: " + dirResult.error().message;
						return false;
					}
				}
				else if (entry.is_regular_file(ec))
				{
					if (auto copyResult = io::file_util::CopyFile(entry.path(), target); !copyResult)
					{
						error = copyResult.error().message;
						return false;
					}
				}
			}
			return true;
		}

		bool CopyIfExists(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error)
		{
			if (!io::file_util::Exists(source))
			{
				error = "Required file does not exist: " + DisplayPath(source);
				return false;
			}
			if (auto result = io::file_util::CopyFile(source, destination); !result)
			{
				error = result.error().message;
				return false;
			}
			return true;
		}

		bool PrunePublishedDevFiles(const std::filesystem::path& packageDir, std::string& error)
		{
			std::error_code ec;
			for (const auto& entry: std::filesystem::recursive_directory_iterator(packageDir, ec))
			{
				if (ec)
				{
					error = "Could not inspect published folder: " + ec.message();
					return false;
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				const std::string ext = LowerAscii(entry.path().extension().generic_string());
				if (ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk")
				{
					std::filesystem::remove(entry.path(), ec);
					if (ec)
					{
						error = "Could not remove dev-only file: " + ec.message();
						return false;
					}
				}
			}
			return true;
		}

		bool VerifyPublishedGame(const std::filesystem::path& packageDir, std::string_view runtimeExecutableName, std::string& error)
		{
			std::vector<std::filesystem::path> requiredFiles{
			        std::filesystem::path(runtimeExecutableName),
			        "data/config/engine.toml",
			        "data/engine.pak",
			        "data/project.pak",
			        "data/scripts/managed/AetherCore.dll",
			        "data/scripts/managed/AetherCore.Interop.dll",
			        "data/scripts/managed/AetherCore.Interop.deps.json",
			        "data/scripts/managed/AetherCore.Interop.runtimeconfig.json",
			        "data/scripts/managed/AetherGame.dll",
			        "data/scripts/managed/AetherGame.deps.json",
			};
			for (const std::filesystem::path& rel: requiredFiles)
			{
				if (!io::file_util::Exists(packageDir / rel))
				{
					error = "Published build is missing: " + rel.generic_string();
					return false;
				}
			}

			const std::string editorExecutable = EditorExecutableName();
			if (editorExecutable != runtimeExecutableName && io::file_util::Exists(packageDir / editorExecutable))
			{
				error = "Published build contains the editor executable: " + editorExecutable;
				return false;
			}

			std::error_code ec;
			for (const auto& entry: std::filesystem::recursive_directory_iterator(packageDir, ec))
			{
				if (ec)
				{
					error = "Could not verify published folder: " + ec.message();
					return false;
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				const std::string ext = LowerAscii(entry.path().extension().generic_string());
				if (ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk" || ext == ".cs" || ext == ".csproj" || ext == ".vcxproj")
				{
					error = "Published build contains a dev/source file: " + DisplayPath(entry.path());
					return false;
				}
				const std::filesystem::path rel = std::filesystem::relative(entry.path(), packageDir, ec);
				if (!ec)
				{
					const std::string relText = LowerAscii(rel.generic_string());
					if (relText.starts_with("debug/") || relText.starts_with("editor/") || relText.starts_with("data/debug/") || relText.starts_with("data/editor/"))
					{
						error = "Published build contains editor/debug artifacts: " + DisplayPath(entry.path());
						return false;
					}
				}
			}
			return true;
		}

		std::optional<std::filesystem::path> FindCMakePackageDirectory(const std::filesystem::path& exeDir)
		{
			const std::filesystem::path configName = exeDir.filename();
			std::vector<std::filesystem::path> candidates;
			candidates.push_back(exeDir.parent_path().parent_path().parent_path() / "package" / configName / "AetherCore");

			std::error_code ec;
			const std::filesystem::path cwd = std::filesystem::current_path(ec);
			if (!ec)
			{
				candidates.push_back(cwd / "package" / configName / "AetherCore");
				candidates.push_back(cwd / "build" / "package" / configName / "AetherCore");
			}

			for (const std::filesystem::path& candidate: candidates)
			{
				if (std::filesystem::is_directory(candidate, ec))
				{
					return candidate;
				}
			}
			return std::nullopt;
		}

		bool CopyRuntimeFromExecutableDir(const std::filesystem::path& exeDir, const std::filesystem::path& packageDir, std::string_view runtimeExecutableName, std::string& error)
		{
			if (!CopyIfExists(exeDir / std::filesystem::path(runtimeExecutableName), packageDir / std::filesystem::path(runtimeExecutableName), error))
			{
				return false;
			}

			std::error_code ec;
			for (const auto& entry: std::filesystem::directory_iterator(exeDir, ec))
			{
				if (ec)
				{
					error = "Could not inspect executable folder: " + ec.message();
					return false;
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				const std::string ext = LowerAscii(entry.path().extension().generic_string());
				if (ext == ".dll" || ext == ".so" || ext == ".dylib")
				{
					if (auto result = io::file_util::CopyFile(entry.path(), packageDir / entry.path().filename()); !result)
					{
						error = result.error().message;
						return false;
					}
				}
			}
			return true;
		}

		bool CopyShippedDataPayload(const std::filesystem::path& exeDataDir, const std::filesystem::path& packageDataDir, std::string& error)
		{
			return CopyIfExists(exeDataDir / "engine.pak", packageDataDir / "engine.pak", error)
			       && CopyIfExists(exeDataDir / "config" / "engine.toml", packageDataDir / "config" / "engine.toml", error)
			       && CopyDirectoryRecursive(exeDataDir / "scripts" / "managed", packageDataDir / "scripts" / "managed", error);
		}

		std::optional<std::filesystem::path> FindAssetPackerExecutable(const std::filesystem::path& exeDir)
		{
			std::error_code ec;
			const std::filesystem::path configName = exeDir.filename();

			std::vector<std::filesystem::path> candidates;
			candidates.push_back(exeDir / "AssetPacker.exe");
			candidates.push_back(exeDir.parent_path().parent_path().parent_path() / "tools" / configName / "AssetPacker.exe");

			const std::filesystem::path cwd = std::filesystem::current_path(ec);
			if (!ec)
			{
				candidates.push_back(cwd / "tools" / configName / "AssetPacker.exe");
				candidates.push_back(cwd / "tools" / "RelWithDebInfo" / "AssetPacker.exe");
				candidates.push_back(cwd / "tools" / "Debug" / "AssetPacker.exe");
				candidates.push_back(cwd / "tools" / "Release" / "AssetPacker.exe");
			}

			for (const std::filesystem::path& candidate: candidates)
			{
				if (io::file_util::Exists(candidate))
				{
					std::error_code canonicalEc;
					return std::filesystem::weakly_canonical(candidate, canonicalEc);
				}
			}
			return std::nullopt;
		}

		std::filesystem::path ProjectDirectoryPath(const std::filesystem::path& root)
		{
			return root / kProjectDirectory;
		}

		std::filesystem::path DescriptorPath(const std::filesystem::path& root)
		{
			return ProjectDirectoryPath(root) / kProjectDescriptor;
		}

		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			return io::file_util::Exists(DescriptorPath(root)) || io::file_util::Exists(root / kProjectDescriptor);
		}

		std::optional<EditorProjectActionResult> ValidateProjectForPackaging(const EditorProjectContext& project)
		{
			if (!project.IsLoaded())
			{
				return EditorProjectActionResult{.succeeded = false, .message = "No project is open."};
			}
			if (!HasProjectDescriptor(project.root))
			{
				return EditorProjectActionResult{.succeeded = false, .message = "Project descriptor is missing: " + DisplayPath(DescriptorPath(project.root))};
			}
			return std::nullopt;
		}

		std::string EscapeXmlAttribute(std::string_view value)
		{
			std::string out;
			for (const char c: value)
			{
				switch (c)
				{
				case '&':
					out += "&amp;";
					break;
				case '<':
					out += "&lt;";
					break;
				case '>':
					out += "&gt;";
					break;
				case '"':
					out += "&quot;";
					break;
				case '\'':
					out += "&apos;";
					break;
				default:
					out += c;
					break;
				}
			}
			return out;
		}

		std::filesystem::path AbsolutePath(std::filesystem::path path)
		{
			if (path.empty())
			{
				return {};
			}
			std::error_code ec;
			std::filesystem::path absolute = std::filesystem::absolute(path, ec);
			return ec ? path.lexically_normal() : absolute.lexically_normal();
		}
	} // namespace

	EditorProjectPublishConfig MakeDefaultEditorProjectPublishConfig()
	{
		EditorProjectPublishConfig config;
		config.executableDir = io::PlatformPaths::GetExecutableDir();
		if (const std::optional<std::filesystem::path> packer = FindAssetPackerExecutable(config.executableDir))
		{
			config.assetPackerExe = *packer;
		}
		if (const std::optional<std::filesystem::path> package = FindCMakePackageDirectory(config.executableDir))
		{
			config.packageTemplateDir = *package;
		}

#ifdef AETHER_DOTNET_EXE
		config.dotnetExe = AETHER_DOTNET_EXE;
#endif
#ifdef AETHER_MANAGED_CONFIG
		config.managedConfig = AETHER_MANAGED_CONFIG;
#endif
#ifdef AETHER_MANAGED_CONFIGDIR
		config.managedConfigDir = AETHER_MANAGED_CONFIGDIR;
#endif
#ifdef AETHER_MANAGED_SDK_PROJECT
		config.managedSdkProject = AbsolutePath(AETHER_MANAGED_SDK_PROJECT);
#endif
#ifdef AETHER_GAME_RUNTIME_EXE_NAME
		config.runtimeExecutableName = AETHER_GAME_RUNTIME_EXE_NAME;
#else
		config.runtimeExecutableName = DefaultRuntimeExecutableName();
#endif
		return config;
	}

	std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject)
	{
		const std::filesystem::path sdkProject = AbsolutePath(managedSdkProject);
		return "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
		       "\n"
		       "  <!--\n"
		       "    Project-owned game scripts. The engine builds this assembly when\n"
		       "    AETHERCORE_PROJECT_DIR points at this project, then loads AetherGame.dll\n"
		       "    through the collectible scripting context.\n"
		       "\n"
		       "    AetherCore is compile-only because the engine already loads the SDK assembly.\n"
		       "    This reference is intentionally absolute so projects created outside the\n"
		       "    engine checkout can still compile from their own scripts folder.\n"
		       "  -->\n"
		       "  <PropertyGroup>\n"
		       "    <AssemblyName>AetherGame</AssemblyName>\n"
		       "    <RootNamespace>AetherGame</RootNamespace>\n"
		       "  </PropertyGroup>\n"
		       "\n"
		       "  <ItemGroup>\n"
		       "    <ProjectReference Include=\""
		       + EscapeXmlAttribute(sdkProject.generic_string())
		       + "\"\n"
		         "                      Private=\"false\"\n"
		         "                      ExcludeAssets=\"runtime\" />\n"
		         "  </ItemGroup>\n"
		         "\n"
		         "</Project>\n";
	}

	EditorProjectActionResult PackProject(const EditorProjectContext& project, const EditorProjectPublishConfig& config)
	{
		if (std::optional<EditorProjectActionResult> validationError = ValidateProjectForPackaging(project))
		{
			return *validationError;
		}
		if (config.assetPackerExe.empty())
		{
			return {.succeeded = false, .message = "Could not find AssetPacker.exe in the editor build output."};
		}

		const std::filesystem::path exeDataDir = config.executableDir / "data";
		const std::filesystem::path outputDir = project.root / "Builds" / "Pack";
		if (auto dirResult = io::file_util::CreateDirectories(outputDir); !dirResult)
		{
			return {.succeeded = false, .message = "Could not create output data directory: " + dirResult.error().message};
		}

		const std::filesystem::path outputPak = outputDir / "project.pak";
		const std::filesystem::path runLog = outputDir / "project.pak.editor.log";
		const std::string command = ShellQuotePath(config.assetPackerExe) + " --project --import-materials " + ShellQuotePath(project.root) + " " + ShellQuotePath(outputPak) + " > " + ShellQuotePath(runLog) + " 2>&1";
		const int exitCode = std::system(command.c_str());
		if (exitCode != 0)
		{
			std::string message = "AssetPacker failed with exit code " + std::to_string(exitCode) + ".";
			const std::string excerpt = ReadLogExcerpt(runLog);
			if (!excerpt.empty())
			{
				message += " " + excerpt;
			}
			return {.succeeded = false, .message = std::move(message), .outputPath = outputPak};
		}

		if (outputDir != exeDataDir)
		{
			if (auto dirResult = io::file_util::CreateDirectories(exeDataDir); dirResult)
			{
				if (auto result = io::file_util::CopyFile(outputPak, exeDataDir / "project.pak"); !result)
				{
					AE_WARN(LogCategory::App, "Failed to copy project.pak: {}", result.error().message);
				}
				if (auto result = io::file_util::CopyFile(outputPak.string() + ".log", exeDataDir / "project.pak.log"); !result)
				{
					AE_WARN(LogCategory::App, "Failed to copy project.pak.log: {}", result.error().message);
				}
				if (auto result = io::file_util::CopyFile(outputPak.string() + ".manifest", exeDataDir / "project.pak.manifest"); !result)
				{
					AE_WARN(LogCategory::App, "Failed to copy project.pak.manifest: {}", result.error().message);
				}
			}
		}

		std::uintmax_t size = 0;
		if (auto fileSizeResult = io::file_util::FileSize(outputPak))
		{
			size = *fileSizeResult;
		}
		AE_INFO(LogCategory::App, "Packed project '{}' to {}", project.name, DisplayPath(outputPak));
		return {.succeeded = true, .message = "Packed project.pak (" + std::to_string(size / 1024) + " KB).", .outputPath = outputPak};
	}

	EditorProjectActionResult PublishProject(const EditorProjectContext& project, const EditorProjectPublishConfig& config)
	{
		if (std::optional<EditorProjectActionResult> validationError = ValidateProjectForPackaging(project))
		{
			return *validationError;
		}

		const std::filesystem::path publishRoot = project.root / "Builds";
		const std::filesystem::path publishDir = publishRoot / PublishPlatformDirectoryName() / "AetherCore";
		if (!PathStartsWith(publishDir, publishRoot))
		{
			return {.succeeded = false, .message = "Refusing to publish outside the project Builds folder.", .outputPath = publishDir};
		}

		std::error_code ec;
		std::filesystem::remove_all(publishRoot, ec);
		if (ec)
		{
			return {.succeeded = false, .message = "Could not clean publish folder: " + ec.message(), .outputPath = publishDir};
		}

		EditorProjectActionResult packResult = PackProject(project, config);
		if (!packResult.succeeded)
		{
			return packResult;
		}

		std::string error;
		const std::filesystem::path exeDataDir = config.executableDir / "data";
		if (!config.packageTemplateDir.empty())
		{
			if (!CopyDirectoryRecursive(config.packageTemplateDir, publishDir, error))
			{
				return {.succeeded = false, .message = "Could not copy package template: " + error, .outputPath = publishDir};
			}
		}
		else
		{
			if (auto dirResult = io::file_util::CreateDirectories(publishDir); !dirResult)
			{
				return {.succeeded = false, .message = "Could not create publish folder: " + dirResult.error().message, .outputPath = publishDir};
			}
			if (!CopyRuntimeFromExecutableDir(config.executableDir, publishDir, config.runtimeExecutableName, error))
			{
				return {.succeeded = false, .message = "Could not copy runtime files: " + error, .outputPath = publishDir};
			}
			if (!CopyShippedDataPayload(exeDataDir, publishDir / "data", error))
			{
				return {.succeeded = false, .message = "Could not copy shipped data: " + error, .outputPath = publishDir};
			}
		}

		if (!CopyIfExists(packResult.outputPath, publishDir / "data" / "project.pak", error))
		{
			return {.succeeded = false, .message = "Could not copy packed project: " + error, .outputPath = publishDir};
		}

		const std::filesystem::path scriptsProject = project.scriptsDir / "AetherGame.csproj";
		if (io::file_util::Exists(scriptsProject))
		{
			if (config.dotnetExe.empty() || config.managedConfig.empty() || config.managedConfigDir.empty())
			{
				return {.succeeded = false, .message = "Project has scripts, but this editor build was not configured with dotnet publishing support.", .outputPath = publishDir};
			}
			const std::filesystem::path artifactsDir = project.root / "Builds" / "Intermediate" / "managed";
			const std::filesystem::path publishLog = project.root / "Builds" / "publish-scripts.log";
			std::filesystem::remove_all(artifactsDir, ec);
			if (ec)
			{
				return {.succeeded = false, .message = "Could not clean script publish intermediates: " + ec.message(), .outputPath = publishDir};
			}
			const std::string command = ShellQuotePath(config.dotnetExe) + " build " + ShellQuotePath(scriptsProject) + " -c " + config.managedConfig + " --nologo -v:m -p:ArtifactsPath=" + ShellQuotePath(artifactsDir);
			if (!RunCommandToLog(command, publishLog, error))
			{
				return {.succeeded = false, .message = "Project script build failed. " + error, .outputPath = publishDir};
			}
			const std::filesystem::path gameOutDir = artifactsDir / "bin" / "AetherGame" / config.managedConfigDir;
			if (!CopyDirectoryRecursive(gameOutDir, publishDir / "data" / "scripts" / "managed", error))
			{
				return {.succeeded = false, .message = "Could not publish project scripts: " + error, .outputPath = publishDir};
			}
		}

		if (!PrunePublishedDevFiles(publishDir, error))
		{
			return {.succeeded = false, .message = error, .outputPath = publishDir};
		}
		if (!VerifyPublishedGame(publishDir, config.runtimeExecutableName, error))
		{
			return {.succeeded = false, .message = error, .outputPath = publishDir};
		}

		AE_INFO(LogCategory::App, "Published project '{}' to {}", project.name, DisplayPath(publishDir));
		return {.succeeded = true, .message = "Published game build.", .outputPath = publishDir};
	}
} // namespace aether::app
