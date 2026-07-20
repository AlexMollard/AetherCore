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
} // namespace aether::app
