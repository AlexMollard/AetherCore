#include "editor/EditorProjectPublisher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "AssetPipeline.hpp"
#include <PakFormat.hpp>

#include "editor/EditorEnginePak.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ShaderCompiler.hpp"
#include "io/FileUtil.hpp"
#include "io/PakBackend.hpp"
#include "io/PlatformPaths.hpp"
#include "io/Process.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

using namespace std::string_view_literals;

// Forward-declared instead of including scene/SceneSerializer.hpp: that header
// transitively pulls <Windows.h>, whose CopyFile macro clobbers the
// io::file_util::CopyFile calls in this file.
namespace aether::app::scene
{
	std::size_t CookProjectBinaries();
}

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kProjectFileName = "ProjectSettings.toml";

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

		// A published game must not carry GFSDK_Aftermath_Lib.x64.dll (or any
		bool IsAftermathRuntimeFile(const std::filesystem::path& path)
		{
			return LowerAscii(path.stem().generic_string()).starts_with("gfsdk_aftermath");
		}

		// redistributable release CRT (msvcp140.dll, no trailing 'd') and never emit these.
		bool IsDebugCrtDll(const std::filesystem::path& path)
		{
			const std::string name = LowerAscii(path.filename().generic_string());
			if (name == "ucrtbased.dll")
			{
				return true;
			}
			bool isCrt = false;
			for (const char* prefix: {"msvcp", "vcruntime", "concrt", "vccorlib"})
			{
				if (name.starts_with(prefix))
				{
					isCrt = true;
					break;
				}
			}
			if (!isCrt || !name.ends_with(".dll"))
			{
				return false;
			}
			for (std::size_t i = 1; i + 1 < name.size(); ++i)
			{
				if (name[i] == 'd' && (std::isdigit(static_cast<unsigned char>(name[i - 1])) != 0) && (name[i + 1] == '.' || name[i + 1] == '_'))
				{
					return true;
				}
			}
			return false;
		}

		// no runtime purpose that leak dev paths - never ship them.
		bool IsPakSidecarFile(const std::filesystem::path& path)
		{
			const std::string name = LowerAscii(path.filename().generic_string());
			return name.ends_with(".pak.log") || name.ends_with(".pak.manifest");
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
#ifdef AETHER_EDITOR_EXE_NAME
			return AETHER_EDITOR_EXE_NAME;
#elif defined(_WIN32)
			return "Editor.exe";
#else
			return "Editor";
#endif
		}

		std::string SanitizePathSegment(std::string value, std::string_view fallback)
		{
			for (char& c: value)
			{
				const unsigned char ch = static_cast<unsigned char>(c);
				if (ch < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
				{
					c = '_';
				}
			}
			while (!value.empty() && (value.back() == ' ' || value.back() == '.'))
			{
				value.pop_back();
			}
			if (value.empty())
			{
				value = fallback;
			}
			return value;
		}

		std::filesystem::path ResolvePublishRoot(const app::EditorProjectContext& project, const EditorProjectPublishOptions& options)
		{
			return options.outputRoot.empty() ? project.root / "Builds" : options.outputRoot;
		}

		std::filesystem::path ResolvePublishDirectory(const app::EditorProjectContext& project, const EditorProjectPublishOptions& options)
		{
			const std::string platform = SanitizePathSegment(options.platformName, PublishPlatformDirectoryName());
			const std::string product = SanitizePathSegment(options.productName, project.name.empty() ? "AetherCore"sv : std::string_view(project.name));
			return ResolvePublishRoot(project, options) / platform / product;
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
				if (ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk" || IsPakSidecarFile(entry.path()))
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

		bool VerifyPublishedGameShaders(const std::filesystem::path& enginePakPath, std::string& error)
		{
			try
			{
				const io::PakBackend enginePak(enginePakPath);
				const auto shaderGlob = enginePak.Glob("shaders/**/*.spv", {});
				if (!shaderGlob.has_value() || shaderGlob->empty())
				{
					error = "Published engine.pak contains no shaders (shaders/*.spv missing): " + DisplayPath(enginePakPath);
					return false;
				}
			}
			catch (const std::exception& ex)
			{
				error = "Could not verify shaders in published engine.pak: " + std::string(ex.what());
				return false;
			}
			return true;
		}

		// nothing about the project being published. The editor never ships this
		bool BakePublishedEngineSettings(const std::filesystem::path& publishedSettingsPath, const std::filesystem::path& projectFile, std::string& error)
		{
			if (auto dirResult = io::file_util::CreateDirectories(publishedSettingsPath.parent_path()); !dirResult)
			{
				error = "Could not create settings output folder: " + dirResult.error().message;
				return false;
			}

			const std::string shippedPath = publishedSettingsPath.string();
			aether::LoadedEngineSettings loaded = aether::EngineSettingsIO::LoadLayered(shippedPath, projectFile);

			// A shipped game runtime has no editor and no Play button, so it must boot
			loaded.base.app.autoplay = true;

			const std::string merged = aether::EngineSettingsIO::Serialize(loaded.base);
			if (auto writeResult = io::file_util::WriteText(publishedSettingsPath, merged); !writeResult)
			{
				error = "Could not bake project startup settings into " + DisplayPath(publishedSettingsPath) + ": " + writeResult.error().message;
				return false;
			}
			AE_INFO(LogCategory::App, "Baked published settings ({}) from project '{}'", DisplayPath(publishedSettingsPath), DisplayPath(projectFile));
			return true;
		}

		bool VerifyPublishedStartupScene(const std::filesystem::path& packageDir, std::string& error)
		{
			const std::filesystem::path settingsPath = packageDir / "data" / "config" / "EngineSettings.toml";
			auto text = io::file_util::ReadText(settingsPath);
			if (!text)
			{
				error = "Could not read published settings to verify the startup scene: " + DisplayPath(settingsPath);
				return false;
			}

			aether::EngineSettings settings{};
			aether::EngineSettingsIO::Apply(*text, settings);
			if (settings.app.startupScene.empty())
			{
				error = "Published build would boot to an empty world: app.startupScene is not set in " + DisplayPath(settingsPath)
				        + ". Set a startup scene in ProjectSettings.toml (e.g. [app] startupscene = \"Level1\") and publish again.";
				return false;
			}
			if (!settings.app.autoplay)
			{
				// A shipped game has no editor and no Play button; without autoplay it
				// boots in edit mode - the scene's main camera is never applied and no
				// scripts run, so the player sees a default camera over an inert scene.
				error = "Published build would boot in edit mode (default camera, no gameplay): app.autoplay is false in " + DisplayPath(settingsPath)
				        + ". BakePublishedEngineSettings must force autoplay = true.";
				return false;
			}

			const std::filesystem::path projectPakPath = packageDir / "data" / "project.pak";
			try
			{
				const io::PakBackend projectPak(projectPakPath);
				const std::string sceneVirtualPath = "scenes/" + settings.app.startupScene + ".scene.toml";
				if (!projectPak.Exists(sceneVirtualPath))
				{
					error = "Published build would boot empty: startup scene '" + settings.app.startupScene + "' (" + sceneVirtualPath + ") was not found in " + DisplayPath(projectPakPath);
					return false;
				}
			}
			catch (const std::exception& ex)
			{
				error = "Could not verify startup scene '" + settings.app.startupScene + "' in " + DisplayPath(projectPakPath) + ": " + std::string(ex.what());
				return false;
			}
			return true;
		}

		bool VerifyPublishedGame(const std::filesystem::path& packageDir, std::string_view runtimeExecutableName, std::string& error)
		{
			const std::vector<std::filesystem::path> requiredFiles{
			        std::filesystem::path(runtimeExecutableName),
			        "data/config/EngineSettings.toml",
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

			if (!VerifyPublishedGameShaders(packageDir / "data" / "engine.pak", error))
			{
				return false;
			}

			if (!VerifyPublishedStartupScene(packageDir, error))
			{
				return false;
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
				if (IsAftermathRuntimeFile(entry.path()))
				{
					error = "Published build contains NVIDIA Aftermath (dev-only, editor-gated): " + DisplayPath(entry.path());
					return false;
				}
				if (IsDebugCrtDll(entry.path()))
				{
					error = "Published build is a Debug build - it ships the non-redistributable debug CRT (" + DisplayPath(entry.path())
					        + "), enables the Vulkan validation layer, and runs unoptimised. Build the editor/runtime in Release or RelWithDebInfo and re-publish for a shippable game.";
					return false;
				}
				if (IsPakSidecarFile(entry.path()))
				{
					error = "Published build contains a packer sidecar (build intermediate): " + DisplayPath(entry.path());
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

		// IsAftermathRuntimeFile). A published game must not carry it.
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
					if (IsAftermathRuntimeFile(entry.path()))
					{
						continue;
					}
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
			return CopyIfExists(exeDataDir / "engine.pak", packageDataDir / "engine.pak", error) && CopyIfExists(exeDataDir / "config" / "EngineSettings.toml", packageDataDir / "config" / "EngineSettings.toml", error)
			       && CopyDirectoryRecursive(exeDataDir / "scripts" / "managed", packageDataDir / "scripts" / "managed", error);
		}

		std::filesystem::path ProjectFilePath(const std::filesystem::path& root)
		{
			return root / kProjectFileName;
		}

		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			return io::file_util::Exists(ProjectFilePath(root));
		}

		std::optional<EditorProjectActionResult> ValidateProjectForPackaging(const app::EditorProjectContext& project)
		{
			if (!project.IsLoaded())
			{
				return EditorProjectActionResult{.succeeded = false, .message = "No project is open."};
			}
			if (!HasProjectDescriptor(project.root))
			{
				return EditorProjectActionResult{.succeeded = false, .message = "Project descriptor is missing: " + DisplayPath(ProjectFilePath(project.root))};
			}
			return std::nullopt;
		}

		std::filesystem::path AbsolutePath(const std::filesystem::path& path)
		{
			if (path.empty())
			{
				return {};
			}
			std::error_code ec;
			const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
			return ec ? path.lexically_normal() : absolute.lexically_normal();
		}

		void ReportProgress(const EditorProjectPublishProgress& progress, const float completion, const std::string_view stage)
		{
			if (progress)
			{
				progress(completion, stage);
			}
		}

		std::string HumanBytes(std::uintmax_t bytes)
		{
			const char* const units[] = {"B", "KB", "MB", "GB"};
			double value = static_cast<double>(bytes);
			int unit = 0;
			while (value >= 1024.0 && unit < 3)
			{
				value /= 1024.0;
				++unit;
			}
			char buffer[48];
			std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.2f %s", value, units[unit]);
			return buffer;
		}

		struct PublishReport
		{
			std::string summary; // one line for the result message
			std::string text;    // full publish-report.txt body
			bool ok = false;
		};

		// Walk the finished package and describe what shipped: total size, file
		// count, the key paks/assemblies, the largest files, and the boot config.
		// Purely observational - never fails the publish.
		PublishReport BuildPublishReport(const std::filesystem::path& packageDir, const app::EditorProjectContext& project, const EditorProjectPublishOptions& options, std::string_view runtimeExecutableName)
		{
			PublishReport report;

			std::vector<std::pair<std::string, std::uintmax_t>> files;
			std::uintmax_t total = 0;
			std::error_code ec;
			for (const auto& entry: std::filesystem::recursive_directory_iterator(packageDir, ec))
			{
				if (ec || !entry.is_regular_file(ec))
				{
					continue;
				}
				const std::uintmax_t size = std::filesystem::file_size(entry.path(), ec);
				if (ec)
				{
					continue;
				}
				const std::filesystem::path rel = std::filesystem::relative(entry.path(), packageDir, ec);
				files.emplace_back(ec ? entry.path().filename().generic_string() : rel.generic_string(), size);
				total += size;
			}

			std::string startupScene = "(unset)";
			bool autoplay = false;
			if (auto settingsText = io::file_util::ReadText(packageDir / "data" / "config" / "EngineSettings.toml"))
			{
				aether::EngineSettings settings{};
				aether::EngineSettingsIO::Apply(*settingsText, settings);
				if (!settings.app.startupScene.empty())
				{
					startupScene = settings.app.startupScene;
				}
				autoplay = settings.app.autoplay;
			}

			const auto sizeOf = [&files](std::string_view rel) -> std::uintmax_t
			{
				for (const auto& [path, size]: files)
				{
					if (path == rel)
					{
						return size;
					}
				}
				return 0;
			};

			std::vector<std::pair<std::string, std::uintmax_t>> largest = files;
			std::sort(largest.begin(), largest.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

			std::string text;
			text += "AetherCore Publish Report\n";
			text += "=========================\n\n";
			text += "Product:       " + (options.productName.empty() ? project.name : options.productName) + "\n";
			text += "Platform:      " + (options.platformName.empty() ? PublishPlatformDirectoryName() : options.platformName) + "\n";
			text += "Runtime:       " + std::string(runtimeExecutableName) + "\n";
			text += "Startup scene: " + startupScene + "  (autoplay: " + (autoplay ? "yes" : "no") + ")\n\n";
			text += "Package\n-------\n";
			text += "Total size:    " + HumanBytes(total) + "\n";
			text += "Files:         " + std::to_string(files.size()) + "\n\n";
			text += "Key payload\n-----------\n";
			text += "engine.pak     " + HumanBytes(sizeOf("data/engine.pak")) + "\n";
			text += "project.pak    " + HumanBytes(sizeOf("data/project.pak")) + "\n";
			text += "runtime exe    " + HumanBytes(sizeOf(std::string(runtimeExecutableName))) + "\n\n";
			text += "Largest files\n-------------\n";
			for (std::size_t i = 0; i < largest.size() && i < 8; ++i)
			{
				char line[512];
				std::snprintf(line, sizeof(line), "  %12s  %s\n", HumanBytes(largest[i].second).c_str(), largest[i].first.c_str());
				text += line;
			}

			report.text = std::move(text);
			report.summary = "Published game build (" + HumanBytes(total) + ", " + std::to_string(files.size()) + " files, startup '" + startupScene + "').";
			report.ok = !files.empty();
			return report;
		}
	} // namespace

	EditorProjectPublishConfig MakeDefaultEditorProjectPublishConfig()
	{
		EditorProjectPublishConfig config;
		config.executableDir = io::PlatformPaths::GetExecutableDir();
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

	namespace
	{
		EditorProjectActionResult PackProjectInternal(const app::EditorProjectContext& project, const EditorProjectPublishConfig& config, bool syncEditorRuntimeProjectPak)
		{
			if (std::optional<EditorProjectActionResult> validationError = ValidateProjectForPackaging(project))
			{
				return *validationError;
			}

			const std::filesystem::path exeDataDir = config.executableDir / "data";
			const std::filesystem::path outputDir = project.root / "Builds" / "Pack";
			if (auto dirResult = io::file_util::CreateDirectories(outputDir); !dirResult)
			{
				return {.succeeded = false, .message = "Could not create output data directory: " + dirResult.error().message};
			}

			// Compile the project's Slang shaders before packing so their .spv
			std::filesystem::path shaderSpirvDir;
			if (CanCompileShaders())
			{
				const std::filesystem::path intermediateShaderDir = ProjectShaderIntermediateDir(project.root);
				std::error_code shaderEc;
				std::filesystem::remove_all(intermediateShaderDir, shaderEc);
				if (shaderEc)
				{
					return {.succeeded = false, .message = "Could not clean shader intermediates: " + shaderEc.message()};
				}

				const ShaderCompileResult shaderResult = CompileProject(project.root);
				if (!shaderResult.ok)
				{
					return {.succeeded = false, .message = "Project shader compile failed: " + shaderResult.message, .outputPath = outputDir / "project.pak"};
				}
				shaderSpirvDir = intermediateShaderDir;
			}

			// Cook fresh scene/prefab binaries into the project so the pak (which
			// packs the whole project tree) ships the fast binary form, not just TOML.
			app::scene::CookProjectBinaries();

			const std::filesystem::path outputPak = outputDir / "project.pak";
			const assetpipeline::PackResult packResult = assetpipeline::PackProject(project.root, outputPak, {.importMaterials = true, .projectLayout = true, .shaderSpirvDir = shaderSpirvDir});
			if (!packResult.ok)
			{
				return {.succeeded = false, .message = packResult.message, .outputPath = outputPak};
			}

			if (syncEditorRuntimeProjectPak && outputDir != exeDataDir)
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
	} // namespace

	EditorProjectPublishOptions MakeDefaultEditorProjectPublishOptions(const app::EditorProjectContext& project)
	{
		EditorProjectPublishOptions options;
		options.outputRoot = project.root / "Builds";
		options.productName = SanitizePathSegment(project.name, "AetherCore");
		options.platformName = PublishPlatformDirectoryName();
		return options;
	}

	EditorProjectActionResult PackProject(const app::EditorProjectContext& project, const EditorProjectPublishConfig& config)
	{
		return PackProjectInternal(project, config, true);
	}

	EditorProjectActionResult PublishProject(const app::EditorProjectContext& project, const EditorProjectPublishConfig& config, const EditorProjectPublishOptions& options, const EditorProjectPublishProgress& progress)
	{
		ReportProgress(progress, 0.02f, "Preparing publish");
		if (std::optional<EditorProjectActionResult> validationError = ValidateProjectForPackaging(project))
		{
			return *validationError;
		}

		const std::filesystem::path publishRoot = ResolvePublishRoot(project, options);
		const std::filesystem::path publishDir = ResolvePublishDirectory(project, options);
		if (options.outputRoot.empty() && !PathStartsWith(publishDir, publishRoot))
		{
			return {.succeeded = false, .message = "Refusing to publish outside the project Builds folder.", .outputPath = publishDir};
		}

		std::error_code ec;
		if (options.cleanOutput)
		{
			ReportProgress(progress, 0.08f, "Cleaning previous output");
			std::filesystem::remove_all(publishDir, ec);
			if (ec)
			{
				return {.succeeded = false, .message = "Could not clean publish folder: " + ec.message(), .outputPath = publishDir};
			}
		}

		ReportProgress(progress, 0.16f, "Packing project assets");
		EditorProjectActionResult packResult = PackProjectInternal(project, config, options.syncEditorRuntimeProjectPak);
		if (!packResult.succeeded)
		{
			return packResult;
		}

		std::string error;
		const std::filesystem::path exeDataDir = config.executableDir / "data";
		ReportProgress(progress, 0.48f, "Staging game runtime");
		if (options.usePackageTemplate && !config.packageTemplateDir.empty())
		{
			if (!CopyDirectoryRecursive(config.packageTemplateDir, publishDir, error))
			{
				return {.succeeded = false, .message = "Could not copy package template: " + error, .outputPath = publishDir};
			}

			const std::filesystem::path templatePak = config.packageTemplateDir / "data" / "engine.pak";
			std::optional<std::uint32_t> runtimePakVersion;
			try
			{
				const io::PakBackend peek(templatePak, /*enforceVersion=*/false);
				runtimePakVersion = peek.DeclaredPipelineVersion();
			}
			catch (const std::exception& ex)
			{
				return {.succeeded = false, .message = "Could not read the runtime package's engine.pak (" + DisplayPath(templatePak) + "): " + ex.what(), .outputPath = publishDir};
			}
			if (runtimePakVersion != PAK_PIPELINE_VERSION)
			{
				const std::string have = runtimePakVersion ? std::to_string(*runtimePakVersion) : std::string("unknown");
				return {.succeeded = false,
				        .message = "Runtime package is out of date: its " + std::string(config.runtimeExecutableName) + " targets pak pipeline v" + have + " but this editor produces v" + std::to_string(PAK_PIPELINE_VERSION)
				                   + ". Rebuild the runtime package (build target PackageGame: cmake --build <builddir> --target PackageGame), then publish again.",
				        .outputPath = publishDir};
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

		// which never opens a project the way the editor does - still resolves
		ReportProgress(progress, 0.63f, "Baking game settings");
		if (!BakePublishedEngineSettings(publishDir / "data" / "config" / "EngineSettings.toml", project.projectFile, error))
		{
			return {.succeeded = false, .message = "Could not bake published settings: " + error, .outputPath = publishDir};
		}

		if (CanBakeEnginePak())
		{
			ReportProgress(progress, 0.70f, "Baking engine assets");
			const EditorProjectActionResult bake = BakeEnginePak(publishDir / "data" / "engine.pak");
			if (!bake.succeeded)
			{
				return {.succeeded = false, .message = "Could not bake engine.pak: " + bake.message, .outputPath = publishDir};
			}
		}

		ReportProgress(progress, 0.78f, "Adding project package");
		if (!CopyIfExists(packResult.outputPath, publishDir / "data" / "project.pak", error))
		{
			return {.succeeded = false, .message = "Could not copy packed project: " + error, .outputPath = publishDir};
		}

		const std::filesystem::path scriptsProject = project.scriptsDir / "AetherGame.csproj";
		if (options.buildProjectScripts && io::file_util::Exists(scriptsProject))
		{
			ReportProgress(progress, 0.84f, "Building game scripts");
			if (config.dotnetExe.empty())
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
			const std::string command =
			        "\"" + config.dotnetExe.string() + "\" build \"" + scriptsProject.string() + "\" -c Release --nologo -v:m -p:DebugSymbols=false -p:DebugType=none -p:Optimize=true -p:ArtifactsPath=\"" + artifactsDir.string() + "\"";
			if (const int rc = io::RunProcessToLog(command, publishLog); rc != 0)
			{
				std::string message = "Project script build failed (exit " + std::to_string(rc) + ").";
				const std::string excerpt = ReadLogExcerpt(publishLog);
				if (!excerpt.empty())
				{
					message += " " + excerpt;
				}
				return {.succeeded = false, .message = std::move(message), .outputPath = publishDir};
			}
			const std::filesystem::path gameOutDir = artifactsDir / "bin" / "AetherGame" / "release";
			if (!CopyDirectoryRecursive(gameOutDir, publishDir / "data" / "scripts" / "managed", error))
			{
				return {.succeeded = false, .message = "Could not publish project scripts: " + error, .outputPath = publishDir};
			}
		}

		ReportProgress(progress, 0.94f, "Finalizing package");
		if (!PrunePublishedDevFiles(publishDir, error))
		{
			return {.succeeded = false, .message = error, .outputPath = publishDir};
		}
		ReportProgress(progress, 0.97f, "Verifying published game");
		if (options.verifyOutput && !VerifyPublishedGame(publishDir, config.runtimeExecutableName, error))
		{
			return {.succeeded = false, .message = error, .outputPath = publishDir};
		}

		// Observability: describe what actually shipped and drop a report next to
		// the build. Best-effort - a report failure never fails the publish.
		std::string message = "Published game build.";
		const PublishReport report = BuildPublishReport(publishDir, project, options, config.runtimeExecutableName);
		if (report.ok)
		{
			message = report.summary;
			if (auto writeResult = io::file_util::WriteText(publishDir / "publish-report.txt", report.text); !writeResult)
			{
				AE_WARN(LogCategory::App, "Could not write publish-report.txt: {}", writeResult.error().message);
			}
		}

		AE_INFO(LogCategory::App, "Published project '{}' to {}", project.name, DisplayPath(publishDir));
		ReportProgress(progress, 1.0f, "Published");
		return {.succeeded = true, .message = std::move(message), .outputPath = publishDir};
	}
} // namespace aether::editor
