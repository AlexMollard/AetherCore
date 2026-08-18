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
} // namespace aether::app
