#include "RuntimeProjectSettings.hpp"

#include <system_error>
#include <array>
#include <algorithm>

#include "project/ProjectCommon.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	std::filesystem::path ResolveEditorScriptBuildDirectory(const std::filesystem::path& artifactsDir, const std::uint32_t processId)
	{
		if (artifactsDir.empty() || !artifactsDir.is_absolute() || processId == 0)
		{
			return {};
		}
		return artifactsDir / "bin" / "AetherGame" / ("debug-" + std::to_string(processId));
	}

	namespace
	{
		std::filesystem::path SettingsIn(const std::filesystem::path& root)
		{
			if (root.empty())
			{
				return {};
			}
			const std::filesystem::path candidate = root / "ProjectSettings.toml";
			std::error_code ec;
			return std::filesystem::exists(candidate, ec) ? candidate : std::filesystem::path{};
		}
	} // namespace

	std::filesystem::path ResolveRuntimeProjectSettings(const RuntimeProjectSettingsInputs& inputs)
	{
		const bool isolated = io::PlatformPaths::IsIsolatedProduct();
		if (isolated)
		{
			if (const auto explicitProject = io::PlatformPaths::GetExplicitProjectDirectory(); !explicitProject.empty())
			{
				return SettingsIn(explicitProject);
			}
		}
		// An explicit override always wins - this is how a dev runs the runtime against a
		// project that is not the one compiled in.
		if (!isolated && !inputs.envProjectDir.empty())
		{
			AE_INFO(LogCategory::App, "Runtime project settings: AETHER_PROJECT_DIR override -> {}", io::PlatformPaths::PathToUtf8(inputs.envProjectDir));
			return SettingsIn(inputs.envProjectDir);
		}

		std::error_code ec;
		if (const std::filesystem::path published = inputs.exeDir / "data" / "config" / "ProjectSettings.toml"; std::filesystem::exists(published, ec))
		{
			AE_INFO(LogCategory::App, "Runtime project settings: shipped ProjectSettings.toml beside the executable.");
			return published;
		}

		// A published package ships its project as data/project.pak and already has that
		// project's settings folded into its own EngineSettings.toml by the publish bake.
		// It must never fall through to the compiled-in project directory: on the machine
		// that built it that directory still exists, and ITS startup scene would silently
		// replace the one that was published.
		if (std::filesystem::exists(inputs.exeDir / "data" / "project.pak", ec))
		{
			// Worth logging loudly rather than returning a silent empty: a DEV build tree that
			// has been packed into looks identical to a package from here, but has no baked
			// settings to fall back on, so it boots an empty world and the only clue is a
			// warning several subsystems later. AETHER_PROJECT_DIR is the way out.
			AE_INFO(LogCategory::App,
			        "Runtime project settings: data/project.pak found next to the executable, so this is treated as a published package and only its "
			        "baked EngineSettings.toml is used. If this is a dev build tree that was packed into, its startup scene will be empty - set "
			        "AETHER_PROJECT_DIR to the project directory to override.");
			return {};
		}

		const std::filesystem::path compiled = isolated ? std::filesystem::path{} : SettingsIn(inputs.compiledDefaultDir);
		AE_INFO(LogCategory::App, "Runtime project settings: {}", compiled.empty() ? std::string{"none found; using the settings beside the executable"} : io::PlatformPaths::PathToUtf8(compiled));
		return compiled;
	}

	Expected<ProjectScriptingSettings> ResolveRuntimeGameplaySettings(const RuntimeProjectSettingsInputs& inputs)
	{
		if (io::PlatformPaths::IsIsolatedProduct())
		{
			if (const auto explicitProject = io::PlatformPaths::GetExplicitProjectDirectory(); !explicitProject.empty())
			{
				// An explicit but missing/malformed project is an error, never permission
				// to select a different backend from the product's baked defaults.
				auto descriptor = project::ReadProjectDescriptor(explicitProject);
				if (!descriptor)
				{
					return std::unexpected(descriptor.error());
				}
				return ProjectScriptingSettings{descriptor->language};
			}
		}
		const auto projectFile = ResolveRuntimeProjectSettings(inputs);
		if (!projectFile.empty())
		{
			const auto sourceRoot = io::PlatformPaths::IsIsolatedProduct() ? io::PlatformPaths::GetExplicitProjectDirectory() : (!inputs.envProjectDir.empty() ? inputs.envProjectDir : inputs.compiledDefaultDir);
			if (!sourceRoot.empty() && projectFile.lexically_normal() == (sourceRoot / "ProjectSettings.toml").lexically_normal())
			{
				// Use the same descriptor/path validation as the editor, including paths.scripts.
				auto descriptor = project::ReadProjectDescriptor(sourceRoot);
				if (!descriptor)
				{
					return std::unexpected(descriptor.error());
				}
				return ProjectScriptingSettings{descriptor->language};
			}
			return ReadProjectScriptingSettings(projectFile);
		}
		// A package without a separate project layer has its authored values baked into
		// EngineSettings.toml. Read only the copy beside this executable.
		std::error_code ec;
		const auto baked = inputs.exeDir / "data" / "config" / "EngineSettings.toml";
		if (std::filesystem::exists(baked, ec))
		{
			return ReadProjectScriptingSettings(baked);
		}
		return ProjectScriptingSettings{};
	}

	Expected<void> ValidateProjectGameplaySelection(const std::filesystem::path& projectRoot, GameplayLanguage activeLanguage, std::optional<std::string_view> configuredLanguage)
	{
		constexpr std::string_view kReopen = "This project's gameplay language changed. Reopen the project from the Launcher to select its gameplay backend.";
		if (configuredLanguage)
		{
			auto configured = ParseGameplayLanguage(*configuredLanguage);
			if (!configured)
			{
				return std::unexpected(configured.error());
			}
			if (*configured != activeLanguage)
			{
				AE_UNEXPECTED(AetherError::Engine(kReopen));
			}
		}
		if (!projectRoot.empty())
		{
			auto descriptor = project::ReadProjectDescriptor(projectRoot);
			if (!descriptor)
			{
				return std::unexpected(descriptor.error());
			}
			if (descriptor->language != activeLanguage)
			{
				AE_UNEXPECTED(AetherError::Engine(kReopen));
			}
		}
		return {};
	}

	std::filesystem::path ResolveRuntimeGameplayModule(const std::filesystem::path& exeDir, GameplayLanguage language)
	{
		if (language == GameplayLanguage::CSharp)
		{
			return exeDir / "data" / "scripts" / "managed" / "AetherGame.dll";
		}
#ifdef _WIN32
		constexpr const char* kModuleName = "aether_gameplay.dll";
#elif defined(__APPLE__)
		constexpr const char* kModuleName = "libaether_gameplay.dylib";
#else
		constexpr const char* kModuleName = "libaether_gameplay.so";
#endif
		return exeDir / "data" / "scripts" / "rust" / kModuleName;
	}

	Expected<std::filesystem::path> ResolveRuntimeManagedDirectory(
	        const std::filesystem::path& exeDir, const std::filesystem::path& workingDir, bool isolated)
	{
		const auto delivered = exeDir / "data/scripts/managed";
		if (isolated)
		{
			std::error_code ec;
			const auto root = std::filesystem::canonical(exeDir, ec);
			if (ec) { AE_UNEXPECTED(AetherError::Engine("Cannot resolve isolated managed bundle root")); }
			for (const auto* name: {"AetherCore.Interop.dll", "AetherCore.Interop.runtimeconfig.json"})
			{
				const auto file = std::filesystem::canonical(delivered / name, ec);
				const auto relative = file.lexically_relative(root);
				if (ec || !std::filesystem::is_regular_file(file, ec) || relative.empty() || relative.is_absolute()
				        || std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
				{
					AE_UNEXPECTED(AetherError::Engine("Isolated product requires delivered data/scripts/managed interop assemblies and runtime configuration; working-directory fallback is disabled"));
				}
			}
			const auto directory = std::filesystem::canonical(delivered, ec);
			if (ec) { AE_UNEXPECTED(AetherError::Engine("Cannot resolve isolated managed directory")); }
			return directory;
		}
		const std::array<std::filesystem::path, 4> candidates{delivered,
		        workingDir / "data/scripts/managed", workingDir / "../data/scripts/managed", workingDir / "../../data/scripts/managed"};
		for (const auto& candidate: candidates)
		{
			std::error_code ec;
			if (std::filesystem::exists(candidate / "AetherCore.Interop.dll", ec)) { return candidate; }
		}
		return delivered;
	}

	bool ProjectScriptsCannotBeLoaded(const RuntimeScriptAssemblyInputs& inputs)
	{
		// Nobody named a project, so whatever is staged beside the runtime IS the game. This is
		// the published-package case and the common one.
		if (inputs.envProjectDir.empty())
		{
			return false;
		}

		std::error_code ec;
		// A project with no scripts of its own has nothing to be missing.
		if (!std::filesystem::exists(inputs.envProjectDir / "scripts" / "AetherGame.csproj", ec))
		{
			return false;
		}

		// Compare what is staged against what this project actually built.
		//
		// Deliberately NOT "is the staged assembly outside the project directory": the editor's
		// correct path also stages outside it, into one directory shared by every project. And
		// deliberately not the data/project.pak beside the executable either - a dev build tree
		// that has ever been packed into has one of those, so it does not distinguish a package
		// from a build tree at all.
		//
		// What does distinguish them is content. If this project has built its own assembly and
		// the staged one is a different file, the staged one belongs to something else.
		const std::filesystem::path builtRoot = inputs.envProjectDir / "Builds" / "Intermediate" / "managed" / "bin" / "AetherGame";
		const std::filesystem::path staged = inputs.managedDir / "AetherGame.dll";
		const auto stagedSize = std::filesystem::file_size(staged, ec);
		if (ec)
		{
			// Nothing staged at all - the caller has bigger problems and will say so itself.
			return false;
		}

		bool sawABuild = false;
		if (std::filesystem::is_directory(builtRoot, ec))
		{
			for (const auto& entry: std::filesystem::directory_iterator(builtRoot, ec))
			{
				std::error_code inner;
				const std::filesystem::path candidate = entry.path() / "AetherGame.dll";
				const auto size = std::filesystem::file_size(candidate, inner);
				if (inner)
				{
					continue;
				}
				sawABuild = true;
				if (size == stagedSize)
				{
					return false; // the staged assembly is one this project built
				}
			}
		}

		// Either this project has never built its scripts, or none of its builds match what is
		// staged. Both mean the same thing for the player: its code is not running.
		(void) sawABuild;
		return true;
	}
} // namespace aether::app
