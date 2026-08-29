#include "editor/publish/PublishVerify.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <system_error>
#include <vector>

#include "io/FileUtil.hpp"
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

		std::vector<std::filesystem::path> RequiredPackageFiles(const std::string_view runtimeExeName)
		{
			return {
			        std::filesystem::path(runtimeExeName),
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
		}

		std::optional<PublishIssue> VerifyShaders(const std::filesystem::path& enginePak)
		{
			try
			{
				const io::PakBackend pak(enginePak);
				const auto shaders = pak.Glob("shaders/**/*.spv", {});
				if (!shaders.has_value() || shaders->empty())
				{
					return PublishIssue{.message = "The published engine.pak contains no compiled shaders.",
					                    .remediation = "Run Recompile Shaders, then publish again."};
				}
			}
			catch (const std::exception& ex)
			{
				return PublishIssue{.message = "Could not read the published engine.pak: " + std::string(ex.what()),
				                    .remediation = "Rebuild the editor so its engine.pak is regenerated, then publish again."};
			}
			return std::nullopt;
		}

		std::optional<PublishIssue> VerifyStartupScene(const std::filesystem::path& packageDir)
		{
			const std::filesystem::path settingsPath = packageDir / "data" / "config" / "EngineSettings.toml";
			auto text = io::file_util::ReadText(settingsPath);
			if (!text)
			{
				return PublishIssue{.message = "Could not read the published settings file.",
				                    .remediation = "Publish again; if it persists, check write permissions on the Builds folder."};
			}

			EngineSettings settings{};
			EngineSettingsIO::Apply(*text, settings);
			if (settings.app.startupScene.empty())
			{
				return PublishIssue{.message = "The published settings have no startup scene.",
				                    .remediation = "Set a startup scene in the Project panel, then publish again."};
			}
			if (!settings.app.autoplay)
			{
				return PublishIssue{.message = "The published settings have autoplay disabled, so the game would boot in edit mode.",
				                    .remediation = "This is an engine bug - the publish bake must force autoplay. Report it."};
			}

			const std::filesystem::path projectPak = packageDir / "data" / "project.pak";
			try
			{
				const io::PakBackend pak(projectPak);
				const std::string sceneVirtualPath = "scenes/" + settings.app.startupScene + ".scene.toml";
				if (!pak.Exists(sceneVirtualPath))
				{
					return PublishIssue{.message = "Startup scene '" + settings.app.startupScene + "' is not in the published project.pak.",
					                    .remediation = "Confirm the scene exists in the project's scenes folder, then publish again."};
				}
				if (const auto bytes = pak.Read(sceneVirtualPath); bytes.has_value())
				{
					const std::string_view sceneToml(reinterpret_cast<const char*>(bytes->data()), bytes->size());
					if (app::scene::SceneTextHasNoCameraSource(sceneToml))
					{
						// Non-fatal: a script may create a camera at runtime.
						AE_WARN(LogCategory::App, "Published startup scene '{}' has no main camera.", settings.app.startupScene);
					}
				}
			}
			catch (const std::exception& ex)
			{
				return PublishIssue{.message = "Could not read the published project.pak: " + std::string(ex.what()),
				                    .remediation = "Publish again to rebuild it."};
			}
			return std::nullopt;
		}
	} // namespace

	bool IsPrunablePublishedFile(const std::filesystem::path& path)
	{
		// The C# compiler. The editor ships it so that a machine with no .NET SDK can build
		// a project's scripts, but a game only ever LOADS the assembly it was given - it has
		// nothing to compile, and Roslyn is ten megabytes in every copy that goes out.
		if (LowerAscii(path.stem().generic_string()).starts_with("microsoft.codeanalysis"))
		{
			return true;
		}

		const std::string ext = LowerAscii(path.extension().generic_string());
		return ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk";
	}

	bool IsForbiddenPublishedFile(const std::filesystem::path& path)
	{
		const std::string ext = LowerAscii(path.extension().generic_string());
		return ext == ".cs" || ext == ".csproj" || ext == ".vcxproj";
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
		for (const std::filesystem::path& rel: RequiredPackageFiles(runtimeExeName))
		{
			if (!io::file_util::Exists(packageDir / rel))
			{
				return PublishIssue{.message = "The published build is missing " + rel.generic_string() + ".",
				                    .remediation = "Publish again; if it persists, rebuild the editor so its bundle is complete."};
			}
		}

		const std::string editorExe = EditorExecutableName();
		if (editorExe != runtimeExeName && io::file_util::Exists(packageDir / editorExe))
		{
			return PublishIssue{.message = "The published build contains the editor executable (" + editorExe + ").",
			                    .remediation = "This is an engine bug - staging must copy only the runtime. Report it."};
		}

		std::error_code ec;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(packageDir, ec))
		{
			if (ec)
			{
				return PublishIssue{.message = "Could not inspect the published folder: " + ec.message(),
				                    .remediation = "Close anything using the Builds folder and publish again."};
			}
			if (!entry.is_regular_file(ec))
			{
				continue;
			}
			const std::filesystem::path& path = entry.path();
			if (IsForbiddenPublishedFile(path) || IsPrunablePublishedFile(path))
			{
				return PublishIssue{.message = "The published build contains a dev/source file: " + path.filename().generic_string() + ".",
				                    .remediation = "This is an engine bug - the prune step should have removed it. Report it."};
			}
			if (IsAftermathRuntimeFile(path))
			{
				return PublishIssue{.message = "The published build contains NVIDIA Aftermath (" + path.filename().generic_string() + "), which is dev-only.",
				                    .remediation = "This is an engine bug - staging must skip it. Report it."};
			}
			if (IsPakSidecarFile(path))
			{
				return PublishIssue{.message = "The published build contains a packer sidecar: " + path.filename().generic_string() + ".",
				                    .remediation = "This is an engine bug - the prune step should have removed it. Report it."};
			}
		}

		if (auto issue = VerifyShaders(packageDir / "data" / "engine.pak"))
		{
			return issue;
		}
		return VerifyStartupScene(packageDir);
	}
} // namespace aether::editor
