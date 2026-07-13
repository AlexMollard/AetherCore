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

namespace aether::app
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

		// NVIDIA Aftermath is a dev-only GPU-crash-diagnostics tool, gated at
		// runtime to editor builds on an NVIDIA device (see VulkanContext.cpp).
		// A published game must not carry GFSDK_Aftermath_Lib.x64.dll (or any
		// GFSDK_Aftermath* file).
		bool IsAftermathRuntimeFile(const std::filesystem::path& path)
		{
			return LowerAscii(path.stem().generic_string()).starts_with("gfsdk_aftermath");
		}

		// Debug CRT DLLs (msvcp140d.dll, msvcp140d_atomic_wait.dll, vcruntime140d.dll,
		// vcruntime140_1d.dll, ucrtbased.dll, ...) are NOT redistributable - Microsoft
		// licenses only the release CRT - and appear only in a Debug build. Detecting
		// one means the whole package is Debug: unshippable (it also enables the Vulkan
		// validation layer and runs unoptimised). Release / RelWithDebInfo link the
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
			// Debug variants append 'd' to the numeric version segment (msvcp140d,
			// vcruntime140_1d, msvcp140d_atomic_wait): a 'd' right after a digit, then '.'/'_'.
			for (std::size_t i = 1; i + 1 < name.size(); ++i)
			{
				if (name[i] == 'd' && (std::isdigit(static_cast<unsigned char>(name[i - 1])) != 0) && (name[i + 1] == '.' || name[i + 1] == '_'))
				{
					return true;
				}
			}
			return false;
		}

		// Packer sidecars (engine.pak.log / engine.pak.manifest / project.pak.*): the
		// human-readable pack log + incremental-repack cache. Build intermediates with
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
#ifdef _WIN32
			return "App.exe";
#else
			return "App";
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

		std::filesystem::path ResolvePublishRoot(const EditorProjectContext& project, const EditorProjectPublishOptions& options)
		{
			return options.outputRoot.empty() ? project.root / "Builds" : options.outputRoot;
		}

		std::filesystem::path ResolvePublishDirectory(const EditorProjectContext& project, const EditorProjectPublishOptions& options)
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

		// Shaders now ship inside engine.pak's "shaders/" prefix (the overlay
		// pipeline - see FileSystem::InitializeDefaultMounts) rather than as a
		// loose shaders/ folder beside the executable, so a file-existence check
		// alone can't catch a shaderless publish (e.g. a dev editor built without
		// AETHER_SHADER_BUILD_DIR baking a fonts-only engine.pak - see
		// EditorEnginePak::CanBakeEnginePak). Mount the shipped engine.pak and
		// glob its "shaders/" prefix directly so verification fails loudly
		// instead of shipping a game that access-violates in BindlessManager.
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

		// The shipped data/config/EngineSettings.toml is a byte-for-byte copy of
		// the engine's generic resources/config/EngineSettings.toml template (see
		// aethercore_add_runtime_payload in src/app/CMakeLists.txt) - it knows
		// nothing about the project being published. The editor never ships this
		// gap: EditorProjectManager::RefreshServices() re-loads settings with the
		// open project's ProjectSettings.toml layered on top
		// (EngineSettingsIO::LoadLayered's layer 3) before ScriptedSceneLayer ever
		// attaches. GameRuntime has no EditorProjectManager - Application always
		// constructs SettingsService from layers 1+2+4 only (see
		// Application::Application(engineConfig) in src/app/Application.cpp) - so
		// without this bake step the published app.startupScene stays whatever
		// the generic template shipped with (empty), and the game boots into an
		// empty world.
		//
		// Bake layers 1 (compiled defaults) + 2 (the just-copied shipped file) +
		// 3 (the project's ProjectSettings.toml) into the published
		// EngineSettings.toml, exactly mirroring what the editor resolves at
		// runtime. Deliberately uses LoadedEngineSettings::base (pre layer-4)
		// rather than .values: baking the *publishing developer's own*
		// UserSettings.toml (layer 4, read from this machine's LocalAppData)
		// into the shipped defaults would leak that developer's local window
		// size/vsync/etc. preferences into every player's fresh install.
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
			// straight into Play mode: Application maps app.autoplay -> PlayState, and
			// only Play mode ticks the C# scripts + animation. The editor default is
			// false (the editor opens in Edit mode and the user presses Play), so force
			// it true for the published build. Without this the game loads the startup
			// scene but sits frozen in Edit mode - nothing updates.
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

		// Reads back the just-baked published settings and, if a startup scene is
		// configured, confirms the scene file is actually present in the shipped
		// project.pak - the same project:// lookup ScriptedSceneLayer::
		// LoadStartupScene / scene::ReadSceneFile perform at boot (see
		// kProjectScenesVfsDir in src/app/scene/SceneSerializer.cpp). Catches a
		// published build that would silently boot into an empty world instead
		// of shipping one.
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
				// No startup scene configured is a deliberate project choice
				// (e.g. a purely script-driven bootstrap) - nothing to verify.
				return true;
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
			std::vector<std::filesystem::path> requiredFiles{
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

		// exeDir here is the running editor's (App.exe's) own executable
		// directory - App and GameRuntime share a build-tree output directory,
		// so App's copy of GFSDK_Aftermath_Lib.x64.dll sits right next to it and
		// would otherwise get swept up by the loop below (see
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
			return CopyIfExists(exeDataDir / "engine.pak", packageDataDir / "engine.pak", error)
			       && CopyIfExists(exeDataDir / "config" / "EngineSettings.toml", packageDataDir / "config" / "EngineSettings.toml", error)
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

		std::optional<EditorProjectActionResult> ValidateProjectForPackaging(const EditorProjectContext& project)
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

	namespace
	{
		EditorProjectActionResult PackProjectInternal(const EditorProjectContext& project, const EditorProjectPublishConfig& config, bool syncEditorRuntimeProjectPak)
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
			// output can be pulled into project.pak's "shaders/" prefix (see
			// PakWriter::AddDirectoryAs / PackOptions::shaderSpirvDir). Unlike
			// the dev hot-reload compile (CompileProjectShadersAndRefreshOverlay,
			// which stays incremental for fast iteration), this is a CLEAN
			// compile - the intermediate dir is wiped first, mirroring the
			// project-scripts dotnet build below, so a .slang source deleted
			// since the last compile can't leave an orphaned .spv behind for
			// AddDirectoryAs to ship into project.pak. When this build has no
			// slangc wired in (CanCompileShaders() == false), this is a
			// graceful no-op and project.pak simply ships with no project
			// shader layer; the published game still runs on engine shaders.
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

			const std::filesystem::path outputPak = outputDir / "project.pak";
			const assetpipeline::PackResult packResult =
			        assetpipeline::PackProject(project.root, outputPak, {.importMaterials = true, .projectLayout = true, .shaderSpirvDir = shaderSpirvDir});
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

	EditorProjectPublishOptions MakeDefaultEditorProjectPublishOptions(const EditorProjectContext& project)
	{
		EditorProjectPublishOptions options;
		options.outputRoot = project.root / "Builds";
		options.productName = SanitizePathSegment(project.name, "AetherCore");
		options.platformName = PublishPlatformDirectoryName();
		return options;
	}

	EditorProjectActionResult PackProject(const EditorProjectContext& project, const EditorProjectPublishConfig& config)
	{
		return PackProjectInternal(project, config, true);
	}

	EditorProjectActionResult PublishProject(const EditorProjectContext& project, const EditorProjectPublishConfig& config, const EditorProjectPublishOptions& options)
	{
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
			std::filesystem::remove_all(publishDir, ec);
			if (ec)
			{
				return {.succeeded = false, .message = "Could not clean publish folder: " + ec.message(), .outputPath = publishDir};
			}
		}

		EditorProjectActionResult packResult = PackProjectInternal(project, config, options.syncEditorRuntimeProjectPak);
		if (!packResult.succeeded)
		{
			return packResult;
		}

		std::string error;
		const std::filesystem::path exeDataDir = config.executableDir / "data";
		if (options.usePackageTemplate && !config.packageTemplateDir.empty())
		{
			if (!CopyDirectoryRecursive(config.packageTemplateDir, publishDir, error))
			{
				return {.succeeded = false, .message = "Could not copy package template: " + error, .outputPath = publishDir};
			}

			// Guard against shipping a stale runtime binary. PackageGame stages
			// AetherGame.exe and engine.pak together from one build, so the template
			// pak's pipeline version is a faithful proxy for the version the runtime
			// EXE expects. BakeEnginePak below rewrites engine.pak at THIS editor's
			// PAK_PIPELINE_VERSION; if the packaged runtime predates it, the fresh pak
			// and the stale exe disagree and the published game rejects its own pak at
			// launch. Fail loudly with an actionable message instead of shipping it.
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

		// Bake the project's ProjectSettings.toml (app.startupScene and friends)
		// into the just-copied published EngineSettings.toml so GameRuntime -
		// which never opens a project the way the editor does - still resolves
		// the same startup scene. Must run after the copy above populates
		// data/config/EngineSettings.toml, and before VerifyPublishedGame.
		if (!BakePublishedEngineSettings(publishDir / "data" / "config" / "EngineSettings.toml", project.projectFile, error))
		{
			return {.succeeded = false, .message = "Could not bake published settings: " + error, .outputPath = publishDir};
		}

		// Prefer a freshly baked engine.pak over the (possibly stale) build-tree copy
		// when this dev editor can bake one. A shipped editor keeps the copied pak.
		if (CanBakeEnginePak())
		{
			const EditorProjectActionResult bake = BakeEnginePak(publishDir / "data" / "engine.pak");
			if (!bake.succeeded)
			{
				return {.succeeded = false, .message = "Could not bake engine.pak: " + bake.message, .outputPath = publishDir};
			}
		}

		if (!CopyIfExists(packResult.outputPath, publishDir / "data" / "project.pak", error))
		{
			return {.succeeded = false, .message = "Could not copy packed project: " + error, .outputPath = publishDir};
		}

		const std::filesystem::path scriptsProject = project.scriptsDir / "AetherGame.csproj";
		if (options.buildProjectScripts && io::file_util::Exists(scriptsProject))
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
			const std::string command = "\"" + config.dotnetExe.string() + "\" build \"" + scriptsProject.string()
			        + "\" -c " + config.managedConfig + " --nologo -v:m -p:ArtifactsPath=\"" + artifactsDir.string() + "\"";
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
		if (options.verifyOutput && !VerifyPublishedGame(publishDir, config.runtimeExecutableName, error))
		{
			return {.succeeded = false, .message = error, .outputPath = publishDir};
		}

		AE_INFO(LogCategory::App, "Published project '{}' to {}", project.name, DisplayPath(publishDir));
		return {.succeeded = true, .message = "Published game build.", .outputPath = publishDir};
	}
} // namespace aether::app
