#include "utils/Logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#endif

namespace aether
{
	namespace
	{
		std::atomic<LogLevel> g_minimumLevel{LogLevel::Verbose};
		constexpr std::uint64_t kInvalidFrameNumber = (std::numeric_limits<std::uint64_t>::max)();
		std::atomic<std::uint64_t> g_frameNumber{kInvalidFrameNumber};

		struct LogEntry
		{
			LogLevel level = LogLevel::Info;
			std::string category;
			std::string message;
			const char* filePath = "";
			std::uint_least32_t line = 0;
			std::time_t timestamp = 0;
			std::uint64_t frameNumber = kInvalidFrameNumber;
			bool showFrame = false;
			bool showSourceLocation = false;
		};

		struct LoggerBackend
		{
			std::mutex mutex;
			std::mutex outputMutex;
			std::condition_variable condition;
			std::condition_variable drainedCondition;
			std::vector<LogEntry> pendingEntries;
			std::thread worker;
			std::ofstream fileStream;
			std::string filePath;
			std::uint64_t nextSequence = 0;
			std::uint64_t completedSequence = 0;
			std::atomic<bool> initialized{false};
			bool stopRequested = false;
			std::terminate_handler previousTerminateHandler = nullptr;
			void (*previousAbortHandler)(int) = nullptr;
#ifdef _WIN32
			LPTOP_LEVEL_EXCEPTION_FILTER previousExceptionFilter = nullptr;
#endif
		};

		std::atomic<bool> g_crashHandlerActive{false};

		LoggerBackend& GetBackend()
		{
			static LoggerBackend backend;
			return backend;
		}

		constexpr const char* kAnsiReset = "\x1b[0m";
		constexpr const char* kAnsiGray = "\x1b[90m";
		constexpr const char* kAnsiYellow = "\x1b[33m";
		constexpr const char* kAnsiRed = "\x1b[31m";
		constexpr const char* kAnsiCyan = "\x1b[96m";
		constexpr const char* kAnsiGreen = "\x1b[92m";
		constexpr const char* kAnsiBoldYellow = "\x1b[1;33m";
		constexpr const char* kAnsiBoldRed = "\x1b[1;31m";
		constexpr const char* kAnsiBright = "\x1b[97m";

		const char* ToLevelName(const LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Verbose:
					return "VERB";
				case LogLevel::Info:
					return "INFO";
				case LogLevel::Warn:
					return "WARN";
				case LogLevel::Error:
					return "ERRO";
				default:
					return "UNKN";
			}
		}

		const char* ToCategoryName(const LogCategory category)
		{
			switch (category)
			{
				case LogCategory::Engine:
					return "Engine";
				case LogCategory::Window:
					return "Window";
				case LogCategory::Vulkan:
					return "Vulkan";
				case LogCategory::Validation:
					return "Validation";
				case LogCategory::Asset:
					return "Asset";
				case LogCategory::FileSystem:
					return "FileSystem";
				case LogCategory::App:
					return "App";
				case LogCategory::Std:
					return "Std";
				case LogCategory::Animation:
					return "Animation";
				case LogCategory::Render:
					return "Render";
				case LogCategory::Scene:
					return "Scene";
				case LogCategory::Camera:
					return "Camera";
				case LogCategory::UI:
					return "UI";
				case LogCategory::Input:
					return "Input";
				case LogCategory::Unknown:
					return "Unknown";
				default:
					return "Unknown";
			}
		}

		const char* ToAnsiColor(const LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Verbose:
					return kAnsiGray;
				case LogLevel::Info:
					return kAnsiCyan;
				case LogLevel::Warn:
					return kAnsiBoldYellow;
				case LogLevel::Error:
					return kAnsiBoldRed;
				default:
					return kAnsiReset;
			}
		}

		const char* ToMessageColor(const LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Verbose:
					return kAnsiGray;
				case LogLevel::Info:
					return kAnsiCyan;
				case LogLevel::Warn:
					return kAnsiYellow;
				case LogLevel::Error:
					return kAnsiRed;
				default:
					return kAnsiReset;
			}
		}

#ifdef _WIN32
		const char* ExceptionCodeToString(const DWORD code)
		{
			switch (code)
			{
				case EXCEPTION_ACCESS_VIOLATION:
					return "ACCESS_VIOLATION";
				case EXCEPTION_STACK_OVERFLOW:
					return "STACK_OVERFLOW";
				case EXCEPTION_ILLEGAL_INSTRUCTION:
					return "ILLEGAL_INSTRUCTION";
				case EXCEPTION_BREAKPOINT:
					return "BREAKPOINT";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
					return "ARRAY_BOUNDS_EXCEEDED";
				case EXCEPTION_FLT_DIVIDE_BY_ZERO:
					return "FLT_DIVIDE_BY_ZERO";
				case EXCEPTION_INT_DIVIDE_BY_ZERO:
					return "INT_DIVIDE_BY_ZERO";
				case EXCEPTION_INT_OVERFLOW:
					return "INT_OVERFLOW";
				case EXCEPTION_PRIV_INSTRUCTION:
					return "PRIV_INSTRUCTION";
				case EXCEPTION_IN_PAGE_ERROR:
					return "IN_PAGE_ERROR";
				case 0xC0000374:
					return "HEAP_CORRUPTION";
				default:
					return "UNKNOWN";
			}
		}
#endif

		bool ShouldShowSourceLocation(const LogLevel level)
		{
			return level != LogLevel::Info;
		}

		std::string_view BuildTimestamp(const std::time_t nowTime)
		{
			thread_local std::time_t cachedSecond = static_cast<std::time_t>(-1);
			thread_local char cachedTimestamp[9] = "00:00:00";

			if (nowTime == cachedSecond)
			{
				return {cachedTimestamp, 8};
			}

			// Cross-platform time conversion: use thread-local storage on all platforms
			std::tm localTime = {};
#ifdef _WIN32
			localtime_s(&localTime, &nowTime);
#else
			// On POSIX systems, use localtime_r which is thread-safe and doesn't use thread_local storage
			::localtime_r(&nowTime, &localTime);
#endif

			std::snprintf(cachedTimestamp, sizeof(cachedTimestamp), "%02d:%02d:%02d", localTime.tm_hour, localTime.tm_min, localTime.tm_sec);

			cachedSecond = nowTime;
			return {cachedTimestamp, 8};
		}

		std::string_view ExtractFileName(const std::string_view path)
		{
			const std::size_t slashPos = path.find_last_of("/\\");
			if (slashPos == std::string_view::npos)
			{
				return path;
			}

			return path.substr(slashPos + 1);
		}

		void EnableVirtualTerminalProcessing();

		void WriteEntry(const LogEntry& entry, std::ofstream& fileStream)
		{
			const std::string_view timestamp = BuildTimestamp(entry.timestamp);
			const std::string_view fileName = ExtractFileName(entry.filePath);

			std::cerr << kAnsiGray << timestamp << kAnsiReset << " " << ToAnsiColor(entry.level) << ToLevelName(entry.level) << kAnsiReset;

			if (!entry.category.empty())
			{
				std::cerr << " " << kAnsiBright << entry.category << kAnsiReset << ":";
			}

			std::cerr << " " << ToMessageColor(entry.level) << entry.message << kAnsiReset;

			if (entry.showFrame)
			{
				std::cerr << " " << kAnsiGray << "(F" << entry.frameNumber << ")" << kAnsiReset;
			}

			if (entry.showSourceLocation)
			{
				std::cerr << " " << kAnsiGray << "(" << fileName << ":" << entry.line << ")" << kAnsiReset;
			}

			std::cerr << kAnsiReset << "\n";

			if (fileStream.is_open())
			{
				fileStream << timestamp << ' ' << ToLevelName(entry.level);

				if (!entry.category.empty())
				{
					fileStream << ' ' << entry.category << ':';
				}

				fileStream << ' ' << entry.message;

				if (entry.showFrame)
				{
					fileStream << " (F" << entry.frameNumber << ')';
				}

				if (entry.showSourceLocation)
				{
					fileStream << " (" << fileName << ':' << entry.line << ')';
				}

				fileStream << '\n';
			}
		}

		void CrashFlushBestEffort()
		{
			LoggerBackend& backend = GetBackend();
			if (!backend.initialized.load(std::memory_order_relaxed))
			{
				return;
			}

			std::scoped_lock writeLock(backend.outputMutex);
			std::cerr.flush();
			if (backend.fileStream.is_open())
			{
				backend.fileStream.flush();
			}
		}

		void TerminateHandler()
		{
			bool expected = false;
			if (g_crashHandlerActive.compare_exchange_strong(expected, true))
			{
				Logger::Log(LogLevel::Error, LogCategory::Engine, "std::terminate called -- unhandled C++ exception", std::source_location::current());
				CrashFlushBestEffort();
			}
			LoggerBackend& backend = GetBackend();
			if (backend.previousTerminateHandler)
			{
				backend.previousTerminateHandler();
			}
			std::abort();
		}

		void AbortHandler(int /*signal*/)
		{
			bool expected = false;
			if (g_crashHandlerActive.compare_exchange_strong(expected, true))
			{
				Logger::Log(LogLevel::Error, LogCategory::Engine, "Abort signal -- CRT assert or explicit abort()", std::source_location::current());
				CrashFlushBestEffort();
			}
			LoggerBackend& backend = GetBackend();
			std::signal(SIGABRT, backend.previousAbortHandler ? backend.previousAbortHandler : SIG_DFL);
			std::raise(SIGABRT);
		}

#ifdef _WIN32
		LONG WINAPI UnhandledExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
		{
			bool expected = false;
			if (g_crashHandlerActive.compare_exchange_strong(expected, true))
			{
				const DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
				const void* address = pExceptionInfo->ExceptionRecord->ExceptionAddress;
				Logger::Log(LogLevel::Error, LogCategory::Engine, std::format("Unhandled exception 0x{:08X} ({}) at 0x{:016X}", code, ExceptionCodeToString(code), reinterpret_cast<std::uintptr_t>(address)), std::source_location::current());
				CrashFlushBestEffort();
			}
			LoggerBackend& backend = GetBackend();
			if (backend.previousExceptionFilter)
			{
				return backend.previousExceptionFilter(pExceptionInfo);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
#endif

		void ProcessLogQueue(LoggerBackend& backend)
		{
			EnableVirtualTerminalProcessing();

			std::vector<LogEntry> batch;
			batch.reserve(64);
			std::uint64_t processedSequence = 0;

			for (;;)
			{
				{
					std::unique_lock lock(backend.mutex);
					backend.condition.wait(lock, [&backend]() { return backend.stopRequested || !backend.pendingEntries.empty(); });

					if (backend.pendingEntries.empty() && backend.stopRequested)
					{
						break;
					}

					batch.clear();
					batch.swap(backend.pendingEntries);
					processedSequence = backend.nextSequence;
				}

				{
					std::scoped_lock writeLock(backend.outputMutex);
					for (const LogEntry& entry: batch)
					{
						WriteEntry(entry, backend.fileStream);
					}
					std::cerr.flush();
					if (backend.fileStream.is_open())
					{
						backend.fileStream.flush();
					}
				}

				{
					std::scoped_lock lock(backend.mutex);
					backend.completedSequence = processedSequence;
				}

				backend.drainedCondition.notify_all();
			}

			if (backend.fileStream.is_open())
			{
				backend.fileStream.flush();
			}
		}

		void EnsureInitialized()
		{
			LoggerBackend& backend = GetBackend();
			if (backend.initialized.load(std::memory_order_acquire))
			{
				return;
			}

			Logger::Initialize();
		}

		void EnableVirtualTerminalProcessing()
		{
#ifdef _WIN32
			static bool isEnabled = false;
			if (isEnabled)
			{
				return;
			}

			HANDLE consoleHandle = GetStdHandle(STD_ERROR_HANDLE);
			if (consoleHandle == INVALID_HANDLE_VALUE)
			{
				return;
			}

			DWORD mode = 0;
			if (!GetConsoleMode(consoleHandle, &mode))
			{
				return;
			}

			if (!SetConsoleMode(consoleHandle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
			{
				return;
			}

			isEnabled = true;
#endif
		}
	} // namespace

	void Logger::Initialize(const std::string_view filePath)
	{
		LoggerBackend& backend = GetBackend();

		std::scoped_lock lock(backend.mutex);
		if (backend.initialized)
		{
			return;
		}

		backend.filePath = std::string(filePath);
		if (!backend.filePath.empty())
		{
			const std::filesystem::path logPath{backend.filePath};
			if (logPath.has_parent_path())
			{
				std::error_code errorCode;
				std::filesystem::create_directories(logPath.parent_path(), errorCode);
			}

			backend.fileStream.open(logPath, std::ios::out | std::ios::app);
		}

		backend.pendingEntries.reserve(256);
		backend.stopRequested = false;
		backend.completedSequence = 0;
		backend.nextSequence = 0;
		backend.previousTerminateHandler = std::set_terminate(TerminateHandler);
		backend.previousAbortHandler = std::signal(SIGABRT, AbortHandler);
#ifdef _WIN32
		backend.previousExceptionFilter = SetUnhandledExceptionFilter(UnhandledExceptionHandler);
#endif
		backend.initialized.store(true, std::memory_order_release);
		backend.worker = std::thread(ProcessLogQueue, std::ref(backend));
	}

	void Logger::Shutdown()
	{
		LoggerBackend& backend = GetBackend();

		{
			std::scoped_lock lock(backend.mutex);
			if (!backend.initialized.load(std::memory_order_acquire))
			{
				return;
			}

			backend.stopRequested = true;
		}

		backend.condition.notify_one();
		if (backend.worker.joinable() && backend.worker.get_id() != std::this_thread::get_id())
		{
			backend.worker.join();
		}
		else if (backend.worker.joinable())
		{
			backend.worker.detach();
		}

		std::scoped_lock lock(backend.mutex);
		if (backend.fileStream.is_open())
		{
			backend.fileStream.close();
		}

		std::set_terminate(backend.previousTerminateHandler);
		std::signal(SIGABRT, backend.previousAbortHandler ? backend.previousAbortHandler : SIG_DFL);
#ifdef _WIN32
		SetUnhandledExceptionFilter(backend.previousExceptionFilter);
#endif
		backend.previousTerminateHandler = nullptr;
		backend.previousAbortHandler = nullptr;
#ifdef _WIN32
		backend.previousExceptionFilter = nullptr;
#endif

		backend.pendingEntries.clear();
		backend.initialized.store(false, std::memory_order_release);
		backend.stopRequested = false;
	}

	void Logger::Flush()
	{
		LoggerBackend& backend = GetBackend();

		std::unique_lock lock(backend.mutex);
		if (!backend.initialized.load(std::memory_order_acquire))
		{
			return;
		}

		const std::uint64_t targetSequence = backend.nextSequence;
		backend.condition.notify_one();
		backend.drainedCondition.wait(lock, [&backend, targetSequence]() { return backend.completedSequence >= targetSequence; });
	}

	void Logger::SetMinimumLevel(const LogLevel level)
	{
		g_minimumLevel.store(level, std::memory_order_relaxed);
	}

	LogLevel Logger::GetMinimumLevel()
	{
		return g_minimumLevel.load(std::memory_order_relaxed);
	}

	bool Logger::ShouldLog(const LogLevel level)
	{
		const LogLevel minimumLevel = g_minimumLevel.load(std::memory_order_relaxed);
		return static_cast<int>(level) >= static_cast<int>(minimumLevel);
	}

	void Logger::SetFrameNumber(const std::uint64_t frameNumber)
	{
		g_frameNumber.store(frameNumber, std::memory_order_relaxed);
	}

	void Logger::ClearFrameNumber()
	{
		g_frameNumber.store(kInvalidFrameNumber, std::memory_order_relaxed);
	}

	void Logger::Log(const LogLevel level, const LogCategory category, const std::string_view message, const std::source_location& location)
	{
		Log(level, ToCategoryName(category), message, location);
	}

	void Logger::Log(const LogLevel level, const std::string_view category, const std::string_view message, const std::source_location& location)
	{
		if (!ShouldLog(level))
		{
			return;
		}

		EnsureInitialized();
		LoggerBackend& backend = GetBackend();
		const std::uint64_t frameNumber = g_frameNumber.load(std::memory_order_relaxed);
		const bool shouldShowFrame = frameNumber != kInvalidFrameNumber;
		const bool shouldShowSourceLocation = ShouldShowSourceLocation(level);

		LogEntry entry;
		entry.level = level;
		entry.category.assign(category);
		entry.message.assign(message);
		entry.filePath = location.file_name();
		entry.line = location.line();
		entry.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		entry.frameNumber = frameNumber;
		entry.showFrame = shouldShowFrame;
		entry.showSourceLocation = shouldShowSourceLocation;

		if (level == LogLevel::Error)
		{
			std::scoped_lock writeLock(backend.outputMutex);
			WriteEntry(entry, backend.fileStream);
			std::cerr.flush();
			if (backend.fileStream.is_open())
			{
				backend.fileStream.flush();
			}
			return;
		}

		{
			std::scoped_lock lock(backend.mutex);
			backend.pendingEntries.push_back(std::move(entry));
			++backend.nextSequence;
		}

		backend.condition.notify_one();
	}

} // namespace aether
