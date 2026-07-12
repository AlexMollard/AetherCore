#pragma once

#include <filesystem>
#include <string>

namespace aether::io
{
	// Sentinel exit codes returned by the process helpers below.
	inline constexpr int kProcessLaunchFailed = -1; // the shell/process could not be started
	inline constexpr int kProcessTimedOut = -2;     // the process exceeded its timeout and its tree was killed

	// Default wall-clock budget for a child process. Generous enough for a cold
	// `dotnet restore` + build, but bounded so nothing can hang forever.
	inline constexpr int kDefaultProcessTimeoutMs = 180000; // 3 minutes

	// Wraps a shell command so it survives cmd.exe's /c quote handling on Windows.
	// On Windows, cmd strips the first and last quote of the command line; a command
	// that itself starts and ends with quoted paths therefore loses them. Wrapping
	// the whole command in an extra outer quote pair makes the strip a no-op. On
	// POSIX shells the command is returned unchanged.
	[[nodiscard]] std::string WrapShellCommand(const std::string& command);

	// Runs `command` through the platform shell, redirecting combined stdout+stderr
	// to `logPath`. Returns the process exit code (0 == success), kProcessLaunchFailed
	// if the process could not be started, or kProcessTimedOut if it exceeded
	// `timeoutMs` (in which case its whole process tree is killed). Creates the log's
	// parent directory if needed.
	[[nodiscard]] int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath, int timeoutMs = kDefaultProcessTimeoutMs);

	// Runs `command` through the platform shell, capturing combined stdout+stderr
	// into `output`. Returns the process exit code, kProcessLaunchFailed on failure to
	// start, or kProcessTimedOut on timeout (tree killed).
	//
	// Robustness: output is captured via a temp FILE, not an inherited pipe. A
	// pipe's read end only sees EOF once every write handle is closed, and tools
	// like `dotnet build` spawn persistent build-server children that inherit that
	// handle and outlive the build - so a naive read-until-EOF hangs forever. A file
	// has no such dependency: the wait completes when the direct child exits,
	// regardless of any lingering grandchildren.
	[[nodiscard]] int RunProcessCapture(const std::string& command, std::string& output, int timeoutMs = kDefaultProcessTimeoutMs);
}
