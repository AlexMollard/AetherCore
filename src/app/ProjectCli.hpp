#pragma once

#include <charconv>
#include <string>
#include <string_view>

namespace aether::app
{
	[[nodiscard]] inline std::string ParseOptionArg(int argc, char** argv, std::string_view option)
	{
		for (int i = 1; i + 1 < argc; ++i)
		{
			if (std::string_view(argv[i]) == option)
			{
				return std::string(argv[i + 1]);
			}
		}
		return {};
	}

	[[nodiscard]] inline std::string ParseProjectArg(int argc, char** argv)
	{
		return ParseOptionArg(argc, argv, "--project");
	}

	// A valueless boolean flag, e.g. --no-validation.
	[[nodiscard]] inline bool HasFlagArg(int argc, char** argv, std::string_view flag)
	{
		for (int i = 1; i < argc; ++i)
		{
			if (std::string_view(argv[i]) == flag)
			{
				return true;
			}
		}
		return false;
	}

	struct AgentLaunchOptions
	{
		bool enabled = false;
		bool background = false;
		std::string name;
		int controlPort = 0;
		std::string error;
	};

	// Editors require an explicit project and control port. Packaged games may use
	// their baked settings and have no editor control endpoint.
	[[nodiscard]] inline AgentLaunchOptions ParseAgentLaunchOptions(int argc, char** argv, bool requireControlPort = true)
	{
		AgentLaunchOptions result;
		result.enabled = HasFlagArg(argc, argv, "--agent");
		result.background = HasFlagArg(argc, argv, "--background");
		const bool hasIdentity = result.background || HasFlagArg(argc, argv, "--agent-name") || HasFlagArg(argc, argv, "--control-port");
		if (!result.enabled)
		{
			if (hasIdentity)
			{
				result.error = "--background, --agent-name and --control-port require --agent";
			}
			return result;
		}
		for (const std::string_view option: {"--agent", "--agent-name", "--project", "--control-port"})
		{
			int count = 0;
			for (int i = 1; i < argc; ++i)
			{
				if (std::string_view(argv[i]) == option)
				{
					++count;
					if (option != "--agent" && (i + 1 == argc || std::string_view(argv[i + 1]).starts_with("--")))
					{
						result.error = std::string(option) + " requires a value";
						return result;
					}
				}
			}
			if (count == 0 && (option == "--control-port" || option == "--project") && !requireControlPort)
			{
				continue;
			}
			if (count != 1)
			{
				result.error = "Agent launch requires exactly one " + std::string(option);
				return result;
			}
		}
		result.name = ParseOptionArg(argc, argv, "--agent-name");
		const std::string project = ParseProjectArg(argc, argv);
		if (result.name.find_first_not_of(" \t\r\n") == std::string::npos || (HasFlagArg(argc, argv, "--project") && project.find_first_not_of(" \t\r\n") == std::string::npos)
		        || result.name.find_first_of("\r\n\t") != std::string::npos)
		{
			result.error = "Agent launch requires a nonblank name (without control characters) and project";
			return result;
		}
		const std::string port = ParseOptionArg(argc, argv, "--control-port");
		if (port.empty() && !requireControlPort)
		{
			return result;
		}
		const auto parsed = std::from_chars(port.data(), port.data() + port.size(), result.controlPort);
		if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || result.controlPort < 1 || result.controlPort > 65535)
		{
			result.error = "--control-port must be an integer from 1 to 65535";
		}
		return result;
	}
	// Whether this run should enable the Vulkan validation layer.
	//
	// `buildDefault` is AE_VALIDATION_DEFAULT_ON: on for Debug, off for RelWithDebInfo.
	// Because that default is now build-config dependent, both directions need an
	// override, and both need to work from either config.
	//
	// PRECEDENCE: --no-validation beats --validation.
	//
	// Off wins deliberately. --no-validation is the escape hatch - it is what you reach
	// for when the layer itself is the problem (it is mutually exclusive with Aftermath,
	// and a layer bug or a false positive can make a session unusable). An escape hatch
	// that a wrapper script can accidentally out-rank by also passing --validation is not
	// an escape hatch. Appending --no-validation to any command line always disables.
	[[nodiscard]] inline bool ResolveValidationEnabled(int argc, char** argv, bool buildDefault)
	{
		if (HasFlagArg(argc, argv, "--no-validation"))
		{
			return false;
		}
		if (HasFlagArg(argc, argv, "--validation"))
		{
			return true;
		}
		return buildDefault;
	}
} // namespace aether::app
