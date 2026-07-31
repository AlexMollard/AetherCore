#include "editor/publish/PublishPlan.hpp"

#include "Defines.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/PlatformPaths.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kProductFallback = "AetherGame";

		std::string HostPlatformName()
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

		std::string HostRuntimeExeName()
		{
#ifdef AETHER_GAME_RUNTIME_EXE_NAME
			return AETHER_GAME_RUNTIME_EXE_NAME;
#elif defined(_WIN32)
			return "AetherGame.exe";
#else
			return "AetherGame";
#endif
		}
	} // namespace

	std::string SanitizePublishSegment(std::string value, const std::string_view fallback)
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
		// An all-separator or all-space name sanitises to underscores or to nothing; neither
		// is a usable folder name, so fall back rather than create a folder called "___".
		if (value.find_first_not_of('_') == std::string::npos)
		{
			value.clear();
		}
		if (value.empty())
		{
			value = std::string(fallback);
		}
		return value;
	}

	PublishPlan MakePublishPlan(const app::EditorProjectContext& project, const PublishEnvironment& environment)
	{
		PublishPlan plan;
		plan.projectRoot = project.root;
		plan.projectFile = project.projectFile;
		plan.scenesDir = project.scenesDir;
		plan.scriptsDir = project.scriptsDir;
		plan.bundleDir = environment.bundleDir;
		plan.runtimeExeName = environment.runtimeExeName;
		plan.configName = environment.configName;
		plan.shippableConfig = environment.shippableConfig;
		plan.platformName = SanitizePublishSegment(environment.platformName, HostPlatformName());
		plan.productName = SanitizePublishSegment(project.name, kProductFallback);
		plan.outputDir = project.root / "Builds" / plan.platformName / plan.productName;
		return plan;
	}

	PublishEnvironment CurrentPublishEnvironment()
	{
		PublishEnvironment environment;
		environment.bundleDir = io::PlatformPaths::GetExecutableDir();
		environment.runtimeExeName = HostRuntimeExeName();
		environment.platformName = HostPlatformName();
		environment.configName = AE_CONFIG_NAME;
		// AE_DEV_TOOLING is 1 for Debug and RelWithDebInfo - exactly the configurations that
		// ship the validation layer and unoptimised code.
		environment.shippableConfig = AE_DEV_TOOLING == 0;
		return environment;
	}
} // namespace aether::editor
