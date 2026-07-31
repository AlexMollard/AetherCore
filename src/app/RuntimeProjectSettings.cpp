#include "RuntimeProjectSettings.hpp"

#include <system_error>

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
			return SettingsIn(inputs.envProjectDir);
		}

		std::error_code ec;
		if (const std::filesystem::path published = inputs.exeDir / "data" / "config" / "ProjectSettings.toml"; std::filesystem::exists(published, ec))
		{
			return published;
		}

		// A published package ships its project as data/project.pak and already has that
		// project's settings folded into its own EngineSettings.toml by the publish bake.
		// It must never fall through to the compiled-in project directory: on the machine
		// that built it that directory still exists, and ITS startup scene would silently
		// replace the one that was published.
		if (std::filesystem::exists(inputs.exeDir / "data" / "project.pak", ec))
		{
			return {};
		}

		return SettingsIn(inputs.compiledDefaultDir);
	}
} // namespace aether::app
