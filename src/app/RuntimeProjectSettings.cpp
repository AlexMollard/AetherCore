#include "RuntimeProjectSettings.hpp"

#include <system_error>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
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
		// An explicit override always wins - this is how a dev runs the runtime against a
		// project that is not the one compiled in.
		if (!inputs.envProjectDir.empty())
		{
			AE_INFO(LogCategory::App, "Runtime project settings: AETHER_PROJECT_DIR override -> {}", inputs.envProjectDir.string());
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

		const std::filesystem::path compiled = SettingsIn(inputs.compiledDefaultDir);
		AE_INFO(LogCategory::App, "Runtime project settings: {}", compiled.empty() ? std::string{"none found; using the settings beside the executable"} : compiled.string());
		return compiled;
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
