#pragma once

#include <filesystem>
#include <string>

namespace aether::io
{
	inline constexpr int kProcessLaunchFailed = -1;
	inline constexpr int kProcessTimedOut = -2;

	inline constexpr int kDefaultProcessTimeoutMs = 180000;

	[[nodiscard]] std::string WrapShellCommand(const std::string& command);

	[[nodiscard]] int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath, int timeoutMs = kDefaultProcessTimeoutMs);

	[[nodiscard]] int RunProcessCapture(const std::string& command, std::string& output, int timeoutMs = kDefaultProcessTimeoutMs);
} // namespace aether::io
