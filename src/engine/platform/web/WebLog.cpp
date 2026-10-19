#include "WebLog.hpp"

#include <cstdio>

namespace aether
{
	// Browser profile owns no logger thread, file handle, or native rotation service.
	bool Logger::ShouldLog(LogLevel level)
	{
		return level >= LogLevel::Info;
	}

	void Logger::Log(LogLevel, LogCategory, std::string_view message, const std::source_location&)
	{
		std::printf("%.*s\n", static_cast<int>(message.size()), message.data());
	}
} // namespace aether
