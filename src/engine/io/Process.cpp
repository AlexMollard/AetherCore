#include "io/Process.hpp"

#include <array>
#include <cstdio>
#include <system_error>

namespace aether::io
{
	std::string WrapShellCommand(const std::string& command)
	{
#ifdef _WIN32
		// cmd.exe /c strips the first and last quote of the command line. Wrapping
		// the whole thing in an extra pair makes that strip restore the original.
		return "\"" + command + "\"";
#else
		return command;
#endif
	}

	int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath)
	{
		std::error_code ec;
		std::filesystem::create_directories(logPath.parent_path(), ec);

		std::string redirect = command + " > \"" + logPath.string() + "\" 2>&1";
		const std::string wrapped = WrapShellCommand(redirect);
		const int rc = std::system(wrapped.c_str());
		return rc;
	}

	int RunProcessCapture(const std::string& command, std::string& output)
	{
		const std::string wrapped = WrapShellCommand(command + " 2>&1");
#ifdef _WIN32
		FILE* pipe = _popen(wrapped.c_str(), "r");
#else
		FILE* pipe = popen(wrapped.c_str(), "r");
#endif
		if (pipe == nullptr)
		{
			return -1;
		}
		std::array<char, 512> buffer{};
		while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
		{
			output += buffer.data();
		}
#ifdef _WIN32
		return _pclose(pipe);
#else
		return pclose(pipe);
#endif
	}
} // namespace aether::io
