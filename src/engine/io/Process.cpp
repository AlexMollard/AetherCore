#include "io/Process.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <cstdlib>
#	include <sys/wait.h>
#endif

namespace aether::io
{
	namespace
	{
		// Read a whole file into a string (empty if it can't be opened).
		std::string SlurpFile(const std::filesystem::path& path)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
			{
				return {};
			}
			std::ostringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}

#ifdef _WIN32
		// Runs `command` via cmd.exe with stdout+stderr redirected to `outFile`,
		// under a kill-on-close Job Object so the entire process tree (including any
		// build-server grandchildren) dies when we're done or on timeout. Returns the
		// exit code, kProcessLaunchFailed, or kProcessTimedOut.
		int RunToFileWindows(const std::string& command, const std::filesystem::path& outFile, int timeoutMs)
		{
			SECURITY_ATTRIBUTES inheritable{};
			inheritable.nLength = sizeof(inheritable);
			inheritable.bInheritHandle = TRUE;

			// The child writes combined stdout+stderr straight into this file.
			HANDLE hOut = CreateFileW(outFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hOut == INVALID_HANDLE_VALUE)
			{
				return kProcessLaunchFailed;
			}
			// Give the child a real (empty) stdin so it never blocks waiting on input.
			HANDLE hIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

			// Kill-on-close job: closing our handle (below) terminates the child and
			// every process it spawned - no orphaned dotnet/MSBuild/VBCSCompiler.
			HANDLE job = CreateJobObjectW(nullptr, nullptr);
			if (job != nullptr)
			{
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
			}

			STARTUPINFOA si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = hOut;
			si.hStdError = hOut;
			si.hStdInput = (hIn != INVALID_HANDLE_VALUE) ? hIn : nullptr;

			// cmd.exe /c "<command>" - the extra quotes match WrapShellCommand so the
			// /c quote-strip restores the command's own inner quotes.
			std::string cmdLine = "cmd.exe /c " + WrapShellCommand(command);
			std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
			mutableCmd.push_back('\0');

			PROCESS_INFORMATION pi{};
			const BOOL started = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE /*inherit handles*/, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);

			int result = kProcessLaunchFailed;
			if (started != 0)
			{
				if (job != nullptr)
				{
					AssignProcessToJobObject(job, pi.hProcess); // capture children before they spawn
				}
				ResumeThread(pi.hThread);

				const DWORD waited = WaitForSingleObject(pi.hProcess, timeoutMs >= 0 ? static_cast<DWORD>(timeoutMs) : INFINITE);
				if (waited == WAIT_TIMEOUT)
				{
					if (job != nullptr)
					{
						TerminateJobObject(job, 1); // kill the whole tree
					}
					else
					{
						TerminateProcess(pi.hProcess, 1);
					}
					WaitForSingleObject(pi.hProcess, 2000);
					result = kProcessTimedOut;
				}
				else
				{
					DWORD code = 0;
					GetExitCodeProcess(pi.hProcess, &code);
					result = static_cast<int>(code);
				}
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}

			if (job != nullptr)
			{
				CloseHandle(job); // kill-on-close reaps any lingering grandchildren
			}
			if (hIn != INVALID_HANDLE_VALUE)
			{
				CloseHandle(hIn);
			}
			CloseHandle(hOut); // flush + release so the file is readable
			return result;
		}
#else
		// POSIX fallback: redirect to the file through the shell. std::system waits
		// only on the direct child, so detached build-server grandchildren can hold
		// the file handle without wedging us (no pipe => no EOF dependency). The
		// timeout is best-effort via coreutils `timeout` when available.
		int RunToFilePosix(const std::string& command, const std::filesystem::path& outFile, int timeoutMs)
		{
			std::string inner = command + " > \"" + outFile.string() + "\" 2>&1";
			if (timeoutMs > 0)
			{
				// `timeout -k` sends SIGKILL a bit after the soft deadline. If the
				// binary is missing the shell reports 127; callers still get output.
				const int secs = (timeoutMs + 999) / 1000;
				inner = "timeout -k 5 " + std::to_string(secs) + " sh -c '" + command + "' > \"" + outFile.string() + "\" 2>&1";
			}
			const int rc = std::system(inner.c_str());
			if (rc == -1)
			{
				return kProcessLaunchFailed;
			}
			// 124 is coreutils `timeout`'s exit code for a killed command.
			if (timeoutMs > 0 && WIFEXITED(rc) && WEXITSTATUS(rc) == 124)
			{
				return kProcessTimedOut;
			}
			return WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
		}
#endif

		int RunToFile(const std::string& command, const std::filesystem::path& outFile, int timeoutMs)
		{
			std::error_code ec;
			std::filesystem::create_directories(outFile.parent_path(), ec);
#ifdef _WIN32
			return RunToFileWindows(command, outFile, timeoutMs);
#else
			return RunToFilePosix(command, outFile, timeoutMs);
#endif
		}
	} // namespace

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

	int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath, int timeoutMs)
	{
		return RunToFile(command, logPath, timeoutMs);
	}

	int RunProcessCapture(const std::string& command, std::string& output, int timeoutMs)
	{
		// Capture through a temp file rather than an inherited pipe: a pipe only
		// reports EOF once ALL write handles close, and dotnet/MSBuild leave
		// persistent build-server children holding that handle, so read-until-EOF
		// hangs forever. A file has no such dependency.
		std::error_code ec;
		std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			tmp = std::filesystem::path("."); // last resort: cwd
		}
		// Unique-enough name without needing <random>: mix the address of a local.
		std::ostringstream name;
		name << "aether_proc_" << static_cast<const void*>(&output) << "_" << std::hash<std::string>{}(command) << ".log";
		tmp /= name.str();

		const int rc = RunToFile(command, tmp, timeoutMs);
		output = SlurpFile(tmp);
		std::filesystem::remove(tmp, ec);
		return rc;
	}
} // namespace aether::io
