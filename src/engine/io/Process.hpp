#pragma once

#include <filesystem>
#include <string>
#include <span>
#include <stop_token>

namespace aether::io
{
	inline constexpr int kProcessLaunchFailed = -1;
	inline constexpr int kProcessTimedOut = -2;
	inline constexpr int kProcessCancelled = -3;

	inline constexpr int kDefaultProcessTimeoutMs = 180000;

	[[nodiscard]] std::string WrapShellCommand(const std::string& command);

	[[nodiscard]] int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath, int timeoutMs = kDefaultProcessTimeoutMs);

	[[nodiscard]] int RunProcessCapture(const std::string& command, std::string& output, int timeoutMs = kDefaultProcessTimeoutMs);
	// Executes an argument vector directly, without a shell. UTF-8 arguments are literal.
	// Cancellation and timeout terminate the owned process tree; pre-cancellation skips launch and log creation.
	[[nodiscard]] int RunProcessArguments(const std::filesystem::path& executable, std::span<const std::string> arguments, const std::filesystem::path& workingDirectory, const std::filesystem::path& logPath, int timeoutMs = kDefaultProcessTimeoutMs, std::stop_token stopToken = {});
} // namespace aether::io
