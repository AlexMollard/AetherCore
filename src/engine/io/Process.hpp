#pragma once

#include <filesystem>
#include <string>

namespace aether::io
{
	// Wraps a shell command so it survives cmd.exe's /c quote handling on Windows.
	// On Windows, cmd strips the first and last quote of the command line; a command
	// that itself starts and ends with quoted paths therefore loses them. Wrapping
	// the whole command in an extra outer quote pair makes the strip a no-op. On
	// POSIX shells the command is returned unchanged.
	[[nodiscard]] std::string WrapShellCommand(const std::string& command);

	// Runs `command` through the platform shell, redirecting combined stdout+stderr
	// to `logPath`. Returns the process exit code (0 == success), or -1 if the shell
	// could not be started. Creates the log's parent directory if needed.
	[[nodiscard]] int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath);

	// Runs `command` through the platform shell, capturing combined stdout+stderr
	// into `output`. Returns the process exit code, or -1 on failure to start.
	[[nodiscard]] int RunProcessCapture(const std::string& command, std::string& output);
}
