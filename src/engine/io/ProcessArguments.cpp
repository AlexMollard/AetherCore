#include "io/Process.hpp"

#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <windows.h>
#else
# include <fcntl.h>
# include <signal.h>
# include <sys/wait.h>
# include <unistd.h>
#endif

namespace aether::io
{
	int RunProcessArguments(const std::filesystem::path& executable, std::span<const std::string> arguments, const std::filesystem::path& workingDirectory, const std::filesystem::path& logPath, int timeoutMs)
	{
		if (executable.empty() || timeoutMs < 0) { return kProcessLaunchFailed; }
		for (const auto& arg: arguments) { if (arg.find('\0') != std::string::npos) { return kProcessLaunchFailed; } }
		std::error_code ec;
		std::filesystem::create_directories(logPath.parent_path(), ec);
		if (ec) { return kProcessLaunchFailed; }
#ifdef _WIN32
		const auto widen = [](const std::string& text) -> std::wstring
		{
			if (text.empty()) { return {}; }
			const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (size <= 0) { return {}; }
			std::wstring wide(size, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
			return wide;
		};
		const auto quote = [](const std::wstring& value)
		{
			std::wstring result = L"\"";
			std::size_t slashes = 0;
			for (const wchar_t c: value)
			{
				if (c == L'\\') { ++slashes; continue; }
				result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
				result += c;
				slashes = 0;
			}
			result.append(slashes * 2, L'\\');
			result += L'"';
			return result;
		};
		std::wstring command = quote(executable.wstring());
		for (const auto& arg: arguments)
		{
			const auto wide = widen(arg);
			if (!arg.empty() && wide.empty()) { return kProcessLaunchFailed; }
			command += L" " + quote(wide);
		}
		SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		const HANDLE output = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (output == INVALID_HANDLE_VALUE) { return kProcessLaunchFailed; }
		const HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
		const HANDLE job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (job == nullptr || SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0)
		{
			if (job) { CloseHandle(job); }
			if (input != INVALID_HANDLE_VALUE) { CloseHandle(input); }
			CloseHandle(output);
			return kProcessLaunchFailed;
		}
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdOutput = output;
		startup.hStdError = output;
		startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;
		PROCESS_INFORMATION process{};
		const auto cwd = workingDirectory.wstring();
		const BOOL started = CreateProcessW(executable.wstring().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
		int result = kProcessLaunchFailed;
		if (started)
		{
			if (AssignProcessToJobObject(job, process.hProcess))
			{
				ResumeThread(process.hThread);
				if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeoutMs)) == WAIT_OBJECT_0)
				{
					DWORD code = 0;
					if (GetExitCodeProcess(process.hProcess, &code)) { result = static_cast<int>(code); }
				}
				else
				{
					TerminateJobObject(job, 1);
					WaitForSingleObject(process.hProcess, 2000);
					result = kProcessTimedOut;
				}
			}
			else { TerminateProcess(process.hProcess, 1); }
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
		CloseHandle(job);
		if (input != INVALID_HANDLE_VALUE) { CloseHandle(input); }
		CloseHandle(output);
		return result;
#else
		const int output = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (output < 0) { return kProcessLaunchFailed; }
		std::string program = executable.string();
		std::vector<char*> argv{program.data()};
		for (const auto& arg: arguments) { argv.push_back(const_cast<char*>(arg.c_str())); }
		argv.push_back(nullptr);
		const auto child = fork();
		if (child == 0)
		{
			setpgid(0, 0);
			dup2(output, STDOUT_FILENO);
			dup2(output, STDERR_FILENO);
			close(output);
			const int input = open("/dev/null", O_RDONLY);
			if (input >= 0) { dup2(input, STDIN_FILENO); close(input); }
			if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0) { _exit(127); }
			execvp(program.c_str(), argv.data());
			_exit(127);
		}
		close(output);
		if (child < 0) { return kProcessLaunchFailed; }
		setpgid(child, child);
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		int status = 0;
		for (;;)
		{
			const auto waited = waitpid(child, &status, WNOHANG);
			if (waited == child) { break; }
			if (waited < 0) { return kProcessLaunchFailed; }
			if (std::chrono::steady_clock::now() >= end)
			{
				kill(-child, SIGKILL);
				waitpid(child, &status, 0);
				return kProcessTimedOut;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return WIFEXITED(status) ? (WEXITSTATUS(status) == 127 ? kProcessLaunchFailed : WEXITSTATUS(status)) : 128 + WTERMSIG(status);
#endif
	}
}
