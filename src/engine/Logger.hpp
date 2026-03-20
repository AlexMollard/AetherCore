#pragma once

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include "LogCategory.hpp"

namespace meow
{
	enum class LogLevel
	{
		Verbose = 0,
		Info = 1,
		Warn = 2,
		Error = 3,
	};

	class Logger
	{
	public:
		static void Initialize(std::string_view filePath = "logs/MeowCore.log");
		static void Shutdown();
		static void Flush();
		static void SetMinimumLevel(LogLevel level);
		[[nodiscard]] static LogLevel GetMinimumLevel();
		[[nodiscard]] static bool ShouldLog(LogLevel level);
		static void SetFrameNumber(std::uint64_t frameNumber);
		static void ClearFrameNumber();

		static void Log(LogLevel level, LogCategory category, std::string_view message, const std::source_location& location);
		static void Log(LogLevel level, std::string_view category, std::string_view message, const std::source_location& location);

		template <typename... Args>
		static void VerboseAt(LogCategory category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Verbose))
			{
				return;
			}

			Log(LogLevel::Verbose, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void VerboseAt(std::string_view category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Verbose))
			{
				return;
			}

			Log(LogLevel::Verbose, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void InfoAt(LogCategory category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Info))
			{
				return;
			}

			Log(LogLevel::Info, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void InfoAt(std::string_view category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Info))
			{
				return;
			}

			Log(LogLevel::Info, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void WarnAt(LogCategory category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Warn))
			{
				return;
			}

			Log(LogLevel::Warn, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void WarnAt(std::string_view category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Warn))
			{
				return;
			}

			Log(LogLevel::Warn, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void ErrorAt(LogCategory category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Error))
			{
				return;
			}

			Log(LogLevel::Error, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

		template <typename... Args>
		static void ErrorAt(std::string_view category, const std::source_location& location, std::format_string<Args...> formatText, Args&&... args)
		{
			if (!ShouldLog(LogLevel::Error))
			{
				return;
			}

			Log(LogLevel::Error, category, FormatMessage(formatText, std::forward<Args>(args)...), location);
		}

	private:
		template <typename... Args>
		static std::string FormatMessage(std::format_string<Args...> formatText, Args&&... args)
		{
			return std::format(formatText, std::forward<Args>(args)...);
		}
	};
}

#define VERBOSE(category, formatText, ...) \
	::meow::Logger::VerboseAt(category, std::source_location::current(), formatText __VA_OPT__(,) __VA_ARGS__)

#define INFO(category, formatText, ...) \
	::meow::Logger::InfoAt(category, std::source_location::current(), formatText __VA_OPT__(,) __VA_ARGS__)

#define WARN(category, formatText, ...) \
	::meow::Logger::WarnAt(category, std::source_location::current(), formatText __VA_OPT__(,) __VA_ARGS__)

#define ERROR(category, formatText, ...) \
	::meow::Logger::ErrorAt(category, std::source_location::current(), formatText __VA_OPT__(,) __VA_ARGS__)