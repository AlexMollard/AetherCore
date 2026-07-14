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

	// Returns the value following "--project" on the command line, or "" if the flag is
	// absent or missing its value. The Editor uses this to boot straight into a project:
	// the Launcher passes it when spawning the editor process, and Visual Studio passes
	// it (VS_DEBUGGER_COMMAND_ARGUMENTS) for direct F5 debugging.
	[[nodiscard]] inline std::string ParseProjectArg(int argc, char** argv)
	{
		return ParseOptionArg(argc, argv, "--project");
	}
} // namespace aether::app
