#include "editor/publish/PublishVerify.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <initializer_list>
#include <system_error>
#include <vector>

#include "io/FileUtil.hpp"
#include "ProjectScripting.hpp"
#include "RuntimeProjectSettings.hpp"
#include "io/PakBackend.hpp"
#include "scene/SceneSerializer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		std::string LowerAscii(std::string value)
		{
			std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
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

		std::vector<std::filesystem::path> RequiredGameplayFiles(app::GameplayLanguage language)
		{
			if (language == app::GameplayLanguage::Rust)
			{
				return {app::ResolveRuntimeGameplayModule({}, language)};
			}
			return {
			        "data/scripts/managed/AetherCore.dll",
			        "data/scripts/managed/AetherCore.Interop.dll",
			        "data/scripts/managed/AetherCore.Interop.deps.json",
			        "data/scripts/managed/AetherCore.Interop.runtimeconfig.json",
			        "data/scripts/managed/AetherGame.dll",
			        "data/scripts/managed/AetherGame.deps.json",
			};
		}

		std::optional<PublishIssue> VerifyShaders(const std::filesystem::path& enginePak)
		{
			try
			{
				const io::PakBackend pak(enginePak);
				const auto shaders = pak.Glob("shaders/**/*.spv", {});
				if (!shaders.has_value() || shaders->empty())
				{
					return PublishIssue{.message = "The published engine.pak contains no compiled shaders.", .remediation = "Run Recompile Shaders, then publish again."};
				}
			}
			catch (const std::exception& ex)
			{
				return PublishIssue{.message = "Could not read the published engine.pak: " + std::string(ex.what()), .remediation = "Rebuild the editor so its engine.pak is regenerated, then publish again."};
			}
			return std::nullopt;
		}

		std::optional<PublishIssue> VerifyStartupScene(const std::filesystem::path& packageDir)
		{
			const std::filesystem::path settingsPath = packageDir / "data" / "config" / "EngineSettings.toml";
			auto text = io::file_util::ReadText(settingsPath);
			if (!text)
			{
				return PublishIssue{.message = "Could not read the published settings file.", .remediation = "Publish again; if it persists, check write permissions on the Builds folder."};
			}

			EngineSettings settings{};
			EngineSettingsIO::Apply(*text, settings);
			if (settings.app.startupScene.empty())
			{
				return PublishIssue{.message = "The published settings have no startup scene.", .remediation = "Set a startup scene in the Project panel, then publish again."};
			}
			if (!settings.app.autoplay)
			{
				return PublishIssue{.message = "The published settings have autoplay disabled, so the game would boot in edit mode.", .remediation = "This is an engine bug - the publish bake must force autoplay. Report it."};
			}

			const std::filesystem::path projectPak = packageDir / "data" / "project.pak";
			try
			{
				const io::PakBackend pak(projectPak);
				const std::string sceneVirtualPath = "scenes/" + settings.app.startupScene + ".scene.toml";
				if (!pak.Exists(sceneVirtualPath))
				{
					return PublishIssue{.message = "Startup scene '" + settings.app.startupScene + "' is not in the published project.pak.", .remediation = "Confirm the scene exists in the project's scenes folder, then publish again."};
				}
				const auto bytes = pak.Read(sceneVirtualPath);
				if (!bytes)
				{
					return PublishIssue{.message = "Could not read startup scene '" + settings.app.startupScene + "' from the published project.pak: " + bytes.error().message,
					        .remediation = "Publish again to rebuild the package; check its source assets if the error persists."};
				}
				const std::string_view sceneToml(reinterpret_cast<const char*>(bytes->data()), bytes->size());
				if (app::scene::SceneTextHasNoCameraSource(sceneToml))
				{
					// Non-fatal: a script may create a camera at runtime.
					AE_WARN(LogCategory::App, "Published startup scene '{}' has no main camera.", settings.app.startupScene);
				}
			}
			catch (const std::exception& ex)
			{
				return PublishIssue{.message = "Could not read the published project.pak: " + std::string(ex.what()), .remediation = "Publish again to rebuild it."};
			}
			return std::nullopt;
		}
	} // namespace

	std::optional<PublishIssue> StagePublishedNotices(const std::filesystem::path& bundleDir, const std::filesystem::path& packageDir)
	{
		std::error_code ec;
		std::filesystem::create_directories(packageDir, ec);
		for (const char* file: {"THIRD_PARTY_NOTICES.txt", "DISTRIBUTION.md"})
		{
			if (ec || !std::filesystem::copy_file(bundleDir / file, packageDir / file, std::filesystem::copy_options::overwrite_existing, ec))
			{
				return PublishIssue{.message = "Could not stage distribution notice " + std::string(file) + ": " + ec.message(), .remediation = "Rebuild the editor so its bundle includes distribution notices."};
			}
		}
		if (std::filesystem::exists(bundleDir / "notices", ec))
		{
			std::filesystem::copy(bundleDir / "notices", packageDir / "notices", std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
		}
		if (ec)
		{
			return PublishIssue{.message = "Could not stage dependency notice files: " + ec.message(), .remediation = "Check the editor bundle notices folder and output permissions."};
		}
		if (auto written = io::file_util::WriteTextAtomic(packageDir / "aether-product", "AetherCore delivered product v1\n"); !written)
		{
			return PublishIssue{.message = "Could not write the delivered-product marker: " + written.error().message, .remediation = "Check output permissions and publish again."};
		}
		return std::nullopt;
	}

	std::optional<PublishIssue> VerifyBundledManagedRuntime(const std::filesystem::path& bundleDir)
	{
		const auto hasVersion = [](const std::filesystem::path& directory, const std::initializer_list<std::string_view> requiredFiles)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(directory, ec))
			{
				return false;
			}
			const std::filesystem::directory_iterator end;
			for (std::filesystem::directory_iterator it(directory, ec); !ec && it != end; it.increment(ec))
			{
				std::error_code entryEc;
				if (!it->is_directory(entryEc))
				{
					continue;
				}
				bool complete = true;
				for (const auto fileName: requiredFiles)
				{
					std::error_code fileEc;
					if (!std::filesystem::is_regular_file(it->path() / fileName, fileEc))
					{
						complete = false;
						break;
					}
				}
				if (complete)
				{
					return true;
				}
			}
			return false;
		};
#ifdef _WIN32
		constexpr std::string_view kHostFxr = "hostfxr.dll";
		constexpr std::string_view kCoreClr = "coreclr.dll";
		constexpr std::string_view kHostPolicy = "hostpolicy.dll";
#elif defined(__APPLE__)
		constexpr std::string_view kHostFxr = "libhostfxr.dylib";
		constexpr std::string_view kCoreClr = "libcoreclr.dylib";
		constexpr std::string_view kHostPolicy = "libhostpolicy.dylib";
#else
		constexpr std::string_view kHostFxr = "libhostfxr.so";
		constexpr std::string_view kCoreClr = "libcoreclr.so";
		constexpr std::string_view kHostPolicy = "libhostpolicy.so";
#endif
		const auto runtime = bundleDir / "dotnet/shared/Microsoft.NETCore.App";
		if (!hasVersion(bundleDir / "dotnet/host/fxr", {kHostFxr}) || !hasVersion(runtime, {kCoreClr, kHostPolicy, "System.Private.CoreLib.dll"}))
		{
			return PublishIssue{.message = "C# delivered products require a complete bundled .NET runtime under dotnet/host/fxr and dotnet/shared/Microsoft.NETCore.App; system runtime fallback is unavailable.",
			        .remediation = "Use an engine distribution with its bundled runtime, or stage the supported runtime and its notices in the editor bundle before publishing."};
		}
		return std::nullopt;
	}

	bool IsPrunablePublishedFile(const std::filesystem::path& path)
	{
		// The C# compiler. The editor ships it so that a machine with no .NET SDK can build
		// a project's scripts, but a game only ever LOADS the assembly it was given - it has
		// nothing to compile, and Roslyn is ten megabytes in every copy that goes out.
		if (LowerAscii(path.stem().generic_string()).starts_with("microsoft.codeanalysis") && LowerAscii(path.extension().generic_string()) == ".dll")
		{
			return true;
		}

		const std::string ext = LowerAscii(path.extension().generic_string());
		return ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk";
	}

	bool IsForbiddenPublishedFile(const std::filesystem::path& path)
	{
		const std::string ext = LowerAscii(path.extension().generic_string());
		const std::string name = LowerAscii(path.filename().generic_string());
		return ext == ".cs" || ext == ".csproj" || ext == ".vcxproj" || ext == ".rs" || ext == ".rlib" || ext == ".rmeta" || name == "cargo.toml" || name == "cargo.lock";
	}

	bool IsAftermathRuntimeFile(const std::filesystem::path& path)
	{
		return LowerAscii(path.stem().generic_string()).starts_with("gfsdk_aftermath");
	}

	bool IsPakSidecarFile(const std::filesystem::path& path)
	{
		const std::string name = LowerAscii(path.filename().generic_string());
		return name.ends_with(".pak.log") || name.ends_with(".pak.manifest");
	}

	std::optional<PublishIssue> VerifyPublishedPackage(const std::filesystem::path& packageDir, const std::string_view runtimeExeName)
	{
		std::vector<std::filesystem::path> commonFiles{std::filesystem::path(runtimeExeName), "data/config/EngineSettings.toml", "data/engine.pak", "data/project.pak", "THIRD_PARTY_NOTICES.txt", "DISTRIBUTION.md", "aether-product"};
		if (io::file_util::Exists(packageDir / "dotnet"))
		{
			commonFiles.emplace_back("notices/managed-inventory.json");
			commonFiles.emplace_back("dotnet/LICENSE.txt");
			commonFiles.emplace_back("dotnet/ThirdPartyNotices.txt");
		}
		for (const std::filesystem::path& rel: commonFiles)
		{
			if (!io::file_util::Exists(packageDir / rel))
			{
				return PublishIssue{.message = "The published build is missing " + rel.generic_string() + ".", .remediation = "Publish again; if it persists, rebuild the editor so its bundle is complete."};
			}
		}

		const auto scripting = app::ReadProjectScriptingSettings(packageDir / "data/config/EngineSettings.toml");
		if (!scripting)
		{
			return PublishIssue{.message = "The published scripting settings are invalid: " + scripting.error().message, .remediation = "Select a supported project scripting language, then publish again."};
		}
		if (scripting->language == app::GameplayLanguage::CSharp)
		{
			if (auto issue = VerifyBundledManagedRuntime(packageDir))
			{
				return issue;
			}
		}
		for (const auto& rel: RequiredGameplayFiles(scripting->language))
		{
			if (!io::file_util::Exists(packageDir / rel))
			{
				return PublishIssue{.message = "The published build is missing " + rel.generic_string() + ".", .remediation = "Rebuild the selected gameplay module, then publish again."};
			}
		}

		const std::string editorExe = EditorExecutableName();
		if (editorExe != runtimeExeName && io::file_util::Exists(packageDir / editorExe))
		{
			return PublishIssue{.message = "The published build contains the editor executable (" + editorExe + ").", .remediation = "This is an engine bug - staging must copy only the runtime. Report it."};
		}

		std::error_code ec;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(packageDir, ec))
		{
			if (ec)
			{
				return PublishIssue{.message = "Could not inspect the published folder: " + ec.message(), .remediation = "Close anything using the Builds folder and publish again."};
			}
			if (!entry.is_regular_file(ec))
			{
				continue;
			}
			const std::filesystem::path& path = entry.path();
			if (IsForbiddenPublishedFile(path) || IsPrunablePublishedFile(path))
			{
				return PublishIssue{.message = "The published build contains a dev/source file: " + path.filename().generic_string() + ".", .remediation = "This is an engine bug - the prune step should have removed it. Report it."};
			}
			if (IsAftermathRuntimeFile(path))
			{
				return PublishIssue{.message = "The published build contains NVIDIA Aftermath (" + path.filename().generic_string() + "), which is dev-only.", .remediation = "This is an engine bug - staging must skip it. Report it."};
			}
			if (IsPakSidecarFile(path))
			{
				return PublishIssue{.message = "The published build contains a packer sidecar: " + path.filename().generic_string() + ".", .remediation = "This is an engine bug - the prune step should have removed it. Report it."};
			}
		}

		if (auto issue = VerifyShaders(packageDir / "data" / "engine.pak"))
		{
			return issue;
		}
		return VerifyStartupScene(packageDir);
	}
} // namespace aether::editor
