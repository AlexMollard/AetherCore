#pragma once

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
