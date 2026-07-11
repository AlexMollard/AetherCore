#include "platform/CrashHandler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "io/PlatformPaths.hpp"
#include "utils/LogRingBuffer.hpp"
#include "utils/Logger.hpp"

// clang-format off
#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#	include <DbgHelp.h>
#	include <TlHelp32.h>
#else
#	include <execinfo.h>
#	include <dlfcn.h>
#	include <cxxabi.h>
#endif
// clang-format on

namespace aether
{
	namespace
	{
		std::atomic<bool> g_installed{false};
		std::string g_appName = "AetherCore";
		std::mutex g_writeMutex;
		std::atomic_flag g_crashInProgress = ATOMIC_FLAG_INIT;

		// Engine-supplied context (GPU/driver/scene/build id ...) printed in every
		// report header. Guarded by its own mutex so SetContext is callable from
		// any thread at any time, independent of the crash-write path.
		std::mutex g_contextMutex;
		std::map<std::string, std::string> g_context;

		std::string BuildConfigName()
		{
#ifdef NDEBUG
			return "Release";
#else
			return "Debug";
#endif
		}

		struct StackFrame
		{
			std::uint64_t address = 0;
			std::string symbol;
			std::string file;
			std::string module;
			std::uint32_t line = 0;
			std::uint64_t displacement = 0;
		};

		std::string BuildTimestampForFileName()
		{
			const auto now = std::chrono::system_clock::now();
			const auto timePoint = std::chrono::system_clock::to_time_t(now);

			std::tm localTime{};
#ifdef _WIN32
			localtime_s(&localTime, &timePoint);
#else
			// On POSIX systems, use localtime_r which is thread-safe
			::localtime_r(&timePoint, &localTime);
#endif

			return std::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
		}

		std::filesystem::path BuildCrashBasePath()
		{
			// Crash reports must not land in the game's install directory (a
			// shipped game must not write into its own program directory) --
			// write them under the per-user LocalAppData directory instead.
			// Keyed by executable name (App vs. AetherGame) so the editor and a
			// published game -- which share the same LocalAppData/AetherCore
			// folder but are installed as distinct products -- keep their crash
			// dumps in separate, identifiable folders. This is independent of the
			// caller's Install() appName tag (both currently pass "AetherCore").
			// Falls back to the CWD only if the OS has no resolvable per-user
			// location at all; the non-throwing current_path overload keeps an
			// invalid CWD from throwing on an already-degraded path.
			std::filesystem::path crashDirectory = io::PlatformPaths::GetUserConfigDir();
			if (crashDirectory.empty())
			{
				std::error_code cwdError;
				crashDirectory = std::filesystem::current_path(cwdError);
			}

			std::string exeName = io::PlatformPaths::GetExecutableName();
			if (exeName.empty())
			{
				exeName = "AetherCore";
			}
			crashDirectory = crashDirectory / "crashes" / exeName;

			std::error_code errorCode;
			std::filesystem::create_directories(crashDirectory, errorCode);

			const std::string timestamp = BuildTimestampForFileName();
			const auto threadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
			return crashDirectory / std::format("{}_{}_t{}", g_appName, timestamp, threadHash);
		}

		const char* ToLevelText(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Verbose: return "VERB";
				case LogLevel::Info: return "INFO";
				case LogLevel::Warn: return "WARN";
				case LogLevel::Error: return "ERROR";
			}
			return "?";
		}

		// Build identity + the engine-supplied context map. Cross-platform so a
		// Linux crash report carries the same header shape.
		void WriteBuildAndContext(std::ofstream& output)
		{
			output << std::format("Build: {} ({} {})\n", BuildConfigName(), __DATE__, __TIME__);

			std::map<std::string, std::string> contextCopy;
			{
				std::scoped_lock lock(g_contextMutex);
				contextCopy = g_context;
			}
			for (const auto& [key, value]: contextCopy)
			{
				output << std::format("Context.{}: {}\n", key, value);
			}
		}

		// The last lines the engine logged before the fault - what the engine was
		// actually doing (which scene, which pass, which asset). Pulled from the
		// process-wide LogRingBuffer that also feeds the editor Console.
		void WriteRecentLog(std::ofstream& output, std::size_t maxLines = 60)
		{
			std::vector<LogRingBuffer::Record> records;
			LogRingBuffer::Get().Snapshot(records);
			if (records.empty())
			{
				return;
			}

			const std::size_t begin = records.size() > maxLines ? records.size() - maxLines : 0;
			output << std::format("RecentLog (last {} of {} entries):\n", records.size() - begin, records.size());
			for (std::size_t i = begin; i < records.size(); ++i)
			{
				const LogRingBuffer::Record& record = records[i];
				output << "  " << (record.time.empty() ? "--:--:--" : record.time) << ' ' << ToLevelText(record.level);
				if (!record.category.empty())
				{
					output << ' ' << record.category << ':';
				}
				output << ' ' << record.message << '\n';
			}
			output.flush();
		}

#ifdef _WIN32
		bool IsNoiseFrame(const StackFrame& frame)
		{
			const std::string& name = frame.symbol;
			const std::string& file = frame.file;

			if (name.find("WriteCallStack") != std::string::npos || name.find("WriteTextCrashReport") != std::string::npos || name.find("CaptureCrashArtifacts") != std::string::npos || name.find("SignalHandlerThunk") != std::string::npos
			        || name.find("UnhandledExceptionFilterThunk") != std::string::npos || name.find("TerminateHandlerThunk") != std::string::npos || name.find("WriteAllThreadStacks") != std::string::npos || name.find("WriteMiniDump") != std::string::npos
			        || name.find("WriteSystemInfo") != std::string::npos || name.find("WriteRecentLog") != std::string::npos || name.find("CollectThreadProgramCounters") != std::string::npos)
			{
				return true;
			}

			if (name.find("__scrt_common_main") != std::string::npos || name.find("invoke_main") != std::string::npos || name.find("mainCRTStartup") != std::string::npos || name.find("UnhandledExceptionFilter") != std::string::npos
			        || name.find("BaseThreadInitThunk") != std::string::npos || name.find("RtlUserThreadStart") != std::string::npos || name.find("KiUserExceptionDispatcher") != std::string::npos
			        || name.find("_C_specific_handler") != std::string::npos || name.find("seh_filter_exe") != std::string::npos)
			{
				return true;
			}

			if (file.find("CrashHandler.cpp") != std::string::npos)
			{
				return true;
			}

			return false;
		}

		bool EnsureSymbolsInitialized()
		{
			static std::once_flag symbolInitFlag;
			static bool symbolsInitialized = false;

			std::call_once(symbolInitFlag,
			        []
			        {
				        // SymSetOptions is process-global, so line/undecorate options
				        // apply even if a Vulkan layer (or other component) already owns
				        // the symbol handler.
				        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
				        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE)
				        {
					        symbolsInitialized = true;
					        return;
				        }

				        // SymInitialize fails with ERROR_INVALID_PARAMETER when the
				        // symbol handler is already initialized for this process (common:
				        // a Vulkan validation layer does it first). The symbol APIs still
				        // work in that case - just refresh the module list (picks up
				        // late-loaded driver DLLs) and proceed rather than giving up and
				        // emitting a stackless "<symbol capture failed>" report.
				        if (GetLastError() == ERROR_INVALID_PARAMETER)
				        {
					        SymRefreshModuleList(GetCurrentProcess());
					        symbolsInitialized = true;
				        }
			        });

			return symbolsInitialized;
		}

		const char* ExceptionCodeName(const std::uint32_t code)
		{
			switch (code)
			{
				case EXCEPTION_ACCESS_VIOLATION:
					return "EXCEPTION_ACCESS_VIOLATION";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
					return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
				case EXCEPTION_BREAKPOINT:
					return "EXCEPTION_BREAKPOINT";
				case EXCEPTION_DATATYPE_MISALIGNMENT:
					return "EXCEPTION_DATATYPE_MISALIGNMENT";
				case EXCEPTION_FLT_DIVIDE_BY_ZERO:
					return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
				case EXCEPTION_ILLEGAL_INSTRUCTION:
					return "EXCEPTION_ILLEGAL_INSTRUCTION";
				case EXCEPTION_IN_PAGE_ERROR:
					return "EXCEPTION_IN_PAGE_ERROR";
				case EXCEPTION_INT_DIVIDE_BY_ZERO:
					return "EXCEPTION_INT_DIVIDE_BY_ZERO";
				case EXCEPTION_STACK_OVERFLOW:
					return "EXCEPTION_STACK_OVERFLOW";
				default:
					return "UNKNOWN_EXCEPTION";
			}
		}

		bool IsLikelyNullPointer(const std::uint64_t address)
		{
			return address < 0x10000ULL;
		}

		bool IsLikelyStackPointer(const std::uint64_t value, const std::uint64_t stackPointer)
		{
			constexpr std::uint64_t stackWindow = 0x40000ULL;
			return value >= stackPointer - stackWindow && value <= stackPointer + stackWindow;
		}

		bool IsSameFrame(const StackFrame& lhs, const StackFrame& rhs)
		{
			if (lhs.address != 0 && rhs.address != 0)
			{
				return lhs.address == rhs.address;
			}

			if (!lhs.symbol.empty() && !rhs.symbol.empty() && lhs.symbol == rhs.symbol)
			{
				if (!lhs.file.empty() && !rhs.file.empty())
				{
					return lhs.file == rhs.file && lhs.line == rhs.line;
				}

				return true;
			}

			return false;
		}

		StackFrame ResolveAddressToFrame(const std::uint64_t address)
		{
			StackFrame frame{};
			frame.address = address;

			if (!EnsureSymbolsInitialized())
			{
				return frame;
			}

			std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
			auto symbolInfo = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
			symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbolInfo->MaxNameLen = MAX_SYM_NAME;

			DWORD64 displacement = 0;
			if (SymFromAddr(GetCurrentProcess(), frame.address, &displacement, symbolInfo) == TRUE)
			{
				frame.symbol = symbolInfo->Name;
				frame.displacement = displacement;
			}

			DWORD lineDisplacement = 0;
			IMAGEHLP_LINE64 lineInfo{};
			lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
			if (SymGetLineFromAddr64(GetCurrentProcess(), frame.address, &lineDisplacement, &lineInfo) == TRUE)
			{
				frame.file = lineInfo.FileName;
				frame.line = lineInfo.LineNumber;
			}

			IMAGEHLP_MODULE64 moduleInfo{};
			moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
			if (SymGetModuleInfo64(GetCurrentProcess(), frame.address, &moduleInfo) == TRUE)
			{
				frame.module = moduleInfo.ModuleName;
			}

			return frame;
		}

		std::vector<StackFrame> CaptureStackFrames(EXCEPTION_POINTERS* exceptionPointers)
		{
			std::vector<StackFrame> framesOut;
			if (!EnsureSymbolsInitialized())
			{
				return framesOut;
			}

			if (exceptionPointers != nullptr && exceptionPointers->ContextRecord != nullptr)
			{
				CONTEXT context = *exceptionPointers->ContextRecord;
				STACKFRAME64 stackFrame{};
				DWORD machineType = IMAGE_FILE_MACHINE_AMD64;

				stackFrame.AddrPC.Offset = context.Rip;
				stackFrame.AddrPC.Mode = AddrModeFlat;
				stackFrame.AddrFrame.Offset = context.Rbp;
				stackFrame.AddrFrame.Mode = AddrModeFlat;
				stackFrame.AddrStack.Offset = context.Rsp;
				stackFrame.AddrStack.Mode = AddrModeFlat;

				for (std::size_t i = 0; i < 128; ++i)
				{
					const BOOL advanced = StackWalk64(machineType, GetCurrentProcess(), GetCurrentThread(), &stackFrame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);

					if (advanced == FALSE || stackFrame.AddrPC.Offset == 0)
					{
						break;
					}

					framesOut.push_back(ResolveAddressToFrame(stackFrame.AddrPC.Offset));
				}

				if (!framesOut.empty())
				{
					return framesOut;
				}
			}

			std::array<void*, 96> addresses{};
			const USHORT frameCount = CaptureStackBackTrace(0, static_cast<DWORD>(addresses.size()), addresses.data(), nullptr);
			framesOut.reserve(frameCount);

			for (USHORT frameIndex = 0; frameIndex < frameCount; ++frameIndex)
			{
				framesOut.push_back(ResolveAddressToFrame(reinterpret_cast<std::uint64_t>(addresses[frameIndex])));
			}

			return framesOut;
		}

		void WriteRegisterHints(std::ofstream& output, EXCEPTION_POINTERS* exceptionPointers)
		{
			if (exceptionPointers == nullptr || exceptionPointers->ContextRecord == nullptr)
			{
				return;
			}

			const CONTEXT& context = *exceptionPointers->ContextRecord;
			output << std::format("RegisterSummary: RIP=0x{:X} RSP=0x{:X}\n", static_cast<unsigned long long>(context.Rip), static_cast<unsigned long long>(context.Rsp));

			auto describeRegister = [&](const char* name, const std::uint64_t value)
			{
				if (value == 0)
				{
					output << std::format("  {}: null\n", name);
					return;
				}

				if (IsLikelyStackPointer(value, context.Rsp))
				{
					output << std::format("  {}: stack-like pointer (0x{:X})\n", name, static_cast<unsigned long long>(value));
					return;
				}

				if (IsLikelyNullPointer(value))
				{
					output << std::format("  {}: low memory pointer (0x{:X})\n", name, static_cast<unsigned long long>(value));
					return;
				}

				output << std::format("  {}: 0x{:X}\n", name, static_cast<unsigned long long>(value));
			};

			describeRegister("RAX",
			        context.Rax); // RAX is often used for return values, so it's
			                      // useful to have it in the report
			describeRegister("RBX",
			        context.Rbx); // RBX is a callee-saved register, so it can sometimes hold
			                      // important context across function calls
			describeRegister("RCX",
			        context.Rcx); // RCX is often used for the first integer argument in the
			                      // Windows x64 calling convention, so it can sometimes indicate
			                      // what the crashing function was trying to operate on
			describeRegister("RDX",
			        context.Rdx); // RDX is often used for the second integer argument in the
			                      // Windows x64 calling convention, so it can sometimes indicate
			                      // what the crashing function was trying to operate on
			describeRegister("RBP",
			        context.Rbp); // RBP is the base pointer and can sometimes be used to
			                      // identify stack frames and local variables, although it's not
			                      // always reliable since some functions omit the frame pointer
		}

		void WriteExceptionHints(std::ofstream& output, EXCEPTION_POINTERS* exceptionPointers)
		{
			if (exceptionPointers == nullptr || exceptionPointers->ExceptionRecord == nullptr)
			{
				return;
			}

			const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
			if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
			{
				return;
			}

			const auto operation = record->ExceptionInformation[0] == 0 ? "read" : (record->ExceptionInformation[0] == 1 ? "write" : (record->ExceptionInformation[0] == 8 ? "execute" : "unknown"));
			const auto address = static_cast<std::uint64_t>(record->ExceptionInformation[1]);

			output << std::format("AccessViolation: attempted to {} address 0x{:X}{}\n", operation, static_cast<unsigned long long>(address), IsLikelyNullPointer(address) ? " (likely null/near-null dereference)" : "");
		}

		// Host machine + process facts: which GPU driver DLL, how much RAM, which
		// OS build, the exact command line. A crash on one machine and not another
		// is very often explained here.
		void WriteSystemInfo(std::ofstream& output)
		{
			output << "CommandLine: " << GetCommandLineA() << '\n';

			// RtlGetVersion reports the true OS version (GetVersionEx is app-compat
			// shimmed and lies on modern Windows). OSVERSIONINFOW is layout-compatible
			// with RTL_OSVERSIONINFOW; the void* signature avoids depending on the RTL
			// typedef being visible.
			using RtlGetVersionFn = LONG(WINAPI*)(void*);
			if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
			{
				if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))))
				{
					OSVERSIONINFOW osv{};
					osv.dwOSVersionInfoSize = sizeof(osv);
					if (rtlGetVersion(&osv) == 0)
					{
						output << std::format("OS: Windows {}.{} build {}\n", osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber);
					}
				}
			}

			SYSTEM_INFO systemInfo{};
			GetNativeSystemInfo(&systemInfo);
			output << std::format("CPU: {} logical processors (arch {})\n", systemInfo.dwNumberOfProcessors, systemInfo.wProcessorArchitecture);

			MEMORYSTATUSEX memoryStatus{};
			memoryStatus.dwLength = sizeof(memoryStatus);
			if (GlobalMemoryStatusEx(&memoryStatus) != 0)
			{
				output << std::format("Memory: {} MB total, {} MB available ({}% in use)\n", memoryStatus.ullTotalPhys / (1024 * 1024), memoryStatus.ullAvailPhys / (1024 * 1024), memoryStatus.dwMemoryLoad);
			}
		}

		// Write a real minidump next to the text report. This is the artifact worth
		// keeping: open it in Visual Studio / WinDbg for the full state of every
		// thread, locals, and referenced heap - the thing a text report can never
		// carry. Flags balance usefulness against size (~a few MB typical).
		std::filesystem::path WriteMiniDump(const std::filesystem::path& dumpPath, EXCEPTION_POINTERS* exceptionPointers)
		{
			const HANDLE fileHandle = CreateFileW(dumpPath.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (fileHandle == INVALID_HANDLE_VALUE)
			{
				return {};
			}

			MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
			exceptionInfo.ThreadId = GetCurrentThreadId();
			exceptionInfo.ExceptionPointers = exceptionPointers;
			exceptionInfo.ClientPointers = FALSE;

			const auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithProcessThreadData | MiniDumpWithUnloadedModules);

			const BOOL wrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), fileHandle, dumpType, exceptionPointers != nullptr ? &exceptionInfo : nullptr, nullptr, nullptr);
			CloseHandle(fileHandle);
			return wrote != 0 ? dumpPath : std::filesystem::path{};
		}

		// Walk one suspended thread's stack into raw PC addresses. Kept alloc-light
		// and symbol-free so the caller can resume the thread BEFORE symbolicating -
		// resolving symbols allocates, and holding another thread suspended across a
		// heap allocation is the classic minidump self-deadlock.
		std::vector<std::uint64_t> CollectThreadProgramCounters(HANDLE thread)
		{
			std::vector<std::uint64_t> programCounters;
			CONTEXT context{};
			context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			if (GetThreadContext(thread, &context) == FALSE)
			{
				return programCounters;
			}

			STACKFRAME64 stackFrame{};
			stackFrame.AddrPC.Offset = context.Rip;
			stackFrame.AddrPC.Mode = AddrModeFlat;
			stackFrame.AddrFrame.Offset = context.Rbp;
			stackFrame.AddrFrame.Mode = AddrModeFlat;
			stackFrame.AddrStack.Offset = context.Rsp;
			stackFrame.AddrStack.Mode = AddrModeFlat;

			for (std::size_t i = 0; i < 48; ++i)
			{
				if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &stackFrame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE || stackFrame.AddrPC.Offset == 0)
				{
					break;
				}
				programCounters.push_back(stackFrame.AddrPC.Offset);
			}

			return programCounters;
		}

		// Every OTHER thread's call stack. For a render-thread engine the faulting
		// thread is frequently the victim, not the culprit (a producer parked in a
		// full channel, a worker holding a lock) - so the report is only actionable
		// with all threads visible. The faulting thread itself is dumped separately
		// from the precise exception context.
		void WriteAllThreadStacks(std::ofstream& output)
		{
			if (!EnsureSymbolsInitialized())
			{
				return;
			}

			const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE)
			{
				return;
			}

			const DWORD processId = GetCurrentProcessId();
			const DWORD currentThreadId = GetCurrentThreadId();

			THREADENTRY32 threadEntry{};
			threadEntry.dwSize = sizeof(threadEntry);

			output << "AllThreadStacks (faulting thread shown above):\n";
			int threadCount = 0;
			if (Thread32First(snapshot, &threadEntry) != FALSE)
			{
				do
				{
					if (threadEntry.th32OwnerProcessID != processId || threadEntry.th32ThreadID == currentThreadId)
					{
						continue;
					}

					if (++threadCount > 32)
					{
						output << "  ... additional threads omitted\n";
						break;
					}

					const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadEntry.th32ThreadID);
					if (thread == nullptr)
					{
						continue;
					}

					std::vector<std::uint64_t> programCounters;
					if (SuspendThread(thread) != static_cast<DWORD>(-1))
					{
						programCounters = CollectThreadProgramCounters(thread);
						ResumeThread(thread);
					}
					CloseHandle(thread);

					output << std::format("  Thread {}:\n", threadEntry.th32ThreadID);
					std::size_t shown = 0;
					for (const std::uint64_t programCounter: programCounters)
					{
						const StackFrame frame = ResolveAddressToFrame(programCounter);
						if (IsNoiseFrame(frame))
						{
							continue;
						}
						if (shown++ >= 16)
						{
							break;
						}
						if (!frame.file.empty())
						{
							output << std::format("    {} + 0x{:X} ({}:{}) [{}]\n", frame.symbol.empty() ? "<unknown>" : frame.symbol, static_cast<unsigned long long>(frame.displacement), frame.file, frame.line, frame.module);
						}
						else
						{
							output << std::format("    {} @ 0x{:X} [{}]\n", frame.symbol.empty() ? "<unknown>" : frame.symbol, static_cast<unsigned long long>(frame.address), frame.module.empty() ? "?" : frame.module);
						}
					}
					if (shown == 0)
					{
						output << "    <no symbolic frames>\n";
					}
				} while (Thread32Next(snapshot, &threadEntry) != FALSE);
			}

			CloseHandle(snapshot);
			output.flush();
		}

		void WriteCallStack(std::ofstream& output, EXCEPTION_POINTERS* exceptionPointers, const StackFrame* faultFrame)
		{
			const auto frames = CaptureStackFrames(exceptionPointers);
			if (frames.empty())
			{
				output << "CallStack: <symbol capture failed>\n";
				return;
			}

			std::optional<StackFrame> likelyCrashSite;
			for (const auto& frame: frames)
			{
				if (!IsNoiseFrame(frame))
				{
					likelyCrashSite = frame;
					break;
				}
			}

			if (likelyCrashSite.has_value())
			{
				if (faultFrame == nullptr || !IsSameFrame(*likelyCrashSite, *faultFrame))
				{
					if (!likelyCrashSite->file.empty())
					{
						output << std::format("LikelyCrashSite: {} ({}:{})\n", likelyCrashSite->symbol.empty() ? "<unknown>" : likelyCrashSite->symbol, likelyCrashSite->file, likelyCrashSite->line);
					}
					else
					{
						output << std::format("LikelyCrashSite: {}\n", likelyCrashSite->symbol.empty() ? "<unknown>" : likelyCrashSite->symbol);
					}
				}
			}

			output << "CallStack (filtered):\n";
			std::size_t filteredIndex = 0;
			std::size_t filteredTotal = 0;
			for (const auto& frame: frames)
			{
				if (IsNoiseFrame(frame))
				{
					continue;
				}

				++filteredTotal;
				if (filteredIndex >= 20)
				{
					continue;
				}

				if (!frame.file.empty())
				{
					output << std::format("  [{}] {} + 0x{:X} ({}:{}) [{}]\n", filteredIndex, frame.symbol.empty() ? "<unknown>" : frame.symbol, static_cast<unsigned long long>(frame.displacement), frame.file, frame.line, frame.module.empty() ? "?" : frame.module);
				}
				else
				{
					output << std::format("  [{}] {} @ 0x{:X} [{}]\n", filteredIndex, frame.symbol.empty() ? "<unknown>" : frame.symbol, static_cast<unsigned long long>(frame.address), frame.module.empty() ? "?" : frame.module);
				}

				++filteredIndex;
			}

			if (filteredTotal > filteredIndex)
			{
				output << std::format("AdditionalFramesOmitted: {}\n", filteredTotal - filteredIndex);
			}

			output.flush();
		}
#endif // _WIN32

#ifndef _WIN32
		// Linux/POSIX implementation for stack trace capture
		bool IsNoiseFrame(const StackFrame& frame)
		{
			const std::string& name = frame.symbol;

			// Filter out internal crash handler and signal handling frames
			if (name.find("SignalHandlerThunk") != std::string::npos || name.find("TerminateHandlerThunk") != std::string::npos || name.find("CaptureCrashArtifacts") != std::string::npos || name.find("WriteCallStack") != std::string::npos
			        || name.find("WriteTextCrashReport") != std::string::npos)
			{
				return true;
			}

			return false;
		}

		std::string DemangleSymbol(const std::string& mangled)
		{
			int status = 0;
			char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
			if (status == 0 && demangled != nullptr)
			{
				std::string result(demangled);
				std::free(demangled);
				return result;
			}
			return mangled;
		}

		std::vector<StackFrame> CaptureStackFrames(void*)
		{
			std::vector<StackFrame> framesOut;

			// Capture up to 96 stack frames
			std::array<void*, 96> addresses{};
			int frameCount = backtrace(addresses.data(), static_cast<int>(addresses.size()));

			if (frameCount <= 0)
			{
				return framesOut;
			}

			framesOut.reserve(frameCount);

			for (int i = 0; i < frameCount; ++i)
			{
				StackFrame frame{};
				frame.address = reinterpret_cast<std::uint64_t>(addresses[i]);

				// Try to get symbol information using dladdr
				Dl_info dlInfo{};
				if (dladdr(addresses[i], &dlInfo) != 0)
				{
					if (dlInfo.dli_sname != nullptr)
					{
						frame.symbol = DemangleSymbol(dlInfo.dli_sname);
					}
					if (dlInfo.dli_fname != nullptr)
					{
						frame.module = dlInfo.dli_fname;
					}
				}

				framesOut.push_back(frame);
			}

			return framesOut;
		}

		void WriteCallStack(std::ofstream& output, void*, const StackFrame*)
		{
			const auto frames = CaptureStackFrames(nullptr);
			if (frames.empty())
			{
				output << "CallStack: <symbol capture failed>\n";
				return;
			}

			output << "CallStack (filtered):\n";
			std::size_t filteredIndex = 0;
			std::size_t filteredTotal = 0;

			for (const auto& frame: frames)
			{
				if (IsNoiseFrame(frame))
				{
					continue;
				}

				++filteredTotal;
				if (filteredIndex >= 20)
				{
					continue;
				}

				if (!frame.symbol.empty())
				{
					output << std::format("  [{}] {} @ 0x{:X}\n", filteredIndex, frame.symbol, static_cast<unsigned long long>(frame.address));
				}
				else if (!frame.module.empty())
				{
					output << std::format("  [{}] <{}> @ 0x{:X}\n", filteredIndex, frame.module, static_cast<unsigned long long>(frame.address));
				}
				else
				{
					output << std::format("  [{}] 0x{:X}\n", filteredIndex, static_cast<unsigned long long>(frame.address));
				}

				++filteredIndex;
			}

			if (filteredTotal > filteredIndex)
			{
				output << std::format("AdditionalFramesOmitted: {}\n", filteredTotal - filteredIndex);
			}

			output.flush();
		}

#endif // !_WIN32

		void WriteTextCrashReport(const std::filesystem::path& reportPath,
		        const std::string_view reason,
		        const std::string_view detail,
#ifdef _WIN32
		        EXCEPTION_POINTERS* exceptionPointers,
#else
		        void* /*exceptionPointers*/,
#endif
		        int signalNumber,
		        const std::filesystem::path& dumpPath)
		{
			std::ofstream output(reportPath, std::ios::out | std::ios::trunc);
			if (!output)
			{
				return;
			}

			output << "Application: " << g_appName << '\n';
			output << "Reason: " << reason << '\n';
			if (!detail.empty())
			{
				output << "Detail: " << detail << '\n';
			}
			if (!dumpPath.empty())
			{
				output << "MiniDump: " << dumpPath.string() << "  (open in Visual Studio / WinDbg for full state)\n";
			}
			WriteBuildAndContext(output);
			output << "ThreadIdHash: " << std::hash<std::thread::id>{}(std::this_thread::get_id()) << '\n';

			if (signalNumber != 0)
			{
				output << "Signal: " << signalNumber << '\n';
			}

#ifdef _WIN32
			WriteSystemInfo(output);

			StackFrame faultFrame{};
			bool hasFaultFrame = false;

			if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
			{
				const auto code = exceptionPointers->ExceptionRecord->ExceptionCode;
				output << "ExceptionCode: 0x" << std::hex << code << std::dec << " (" << ExceptionCodeName(code) << ")\n";
				output << "ExceptionAddress: 0x" << std::hex << reinterpret_cast<std::uintptr_t>(exceptionPointers->ExceptionRecord->ExceptionAddress) << std::dec << '\n';

				faultFrame = ResolveAddressToFrame(reinterpret_cast<std::uint64_t>(exceptionPointers->ExceptionRecord->ExceptionAddress));
				hasFaultFrame = true;
				if (!faultFrame.symbol.empty())
				{
					if (!faultFrame.file.empty())
					{
						output << std::format("FaultingInstruction: {} ({}:{})\n", faultFrame.symbol, faultFrame.file, faultFrame.line);
					}
					else
					{
						output << std::format("FaultingInstruction: {}\n", faultFrame.symbol);
					}
				}

				WriteExceptionHints(output, exceptionPointers);
				WriteRegisterHints(output, exceptionPointers);
			}

			WriteCallStack(output, exceptionPointers, hasFaultFrame ? &faultFrame : nullptr);
			WriteAllThreadStacks(output);
#else
			WriteCallStack(output, nullptr, nullptr);
#endif

			WriteRecentLog(output);
		}

		void CaptureCrashArtifacts(const std::string_view reason,
		        const std::string_view detail,
#ifdef _WIN32
		        EXCEPTION_POINTERS* exceptionPointers,
#else
		        void* exceptionPointers,
#endif
		        int signalNumber)
		{
			if (!g_installed.load())
			{
				return;
			}

			std::scoped_lock lock(g_writeMutex);
			const auto basePath = BuildCrashBasePath();

			std::filesystem::path dumpPath;
#ifdef _WIN32
			auto dumpTarget = basePath;
			dumpTarget.replace_extension(".dmp");
			dumpPath = WriteMiniDump(dumpTarget, exceptionPointers); // reliable artifact first
#endif

			auto reportPath = basePath;
			reportPath.replace_extension(".txt");
			WriteTextCrashReport(reportPath, reason, detail, exceptionPointers, signalNumber, dumpPath);

			// Point whoever is watching at the artifacts (std::cerr, not the logger:
			// its worker thread may already be gone by the time we crash).
			std::cerr << "\n[CrashHandler] " << reason << " captured -> " << reportPath.string();
			if (!dumpPath.empty())
			{
				std::cerr << "  (+ minidump " << dumpPath.string() << ")";
			}
			std::cerr << '\n';
			std::cerr.flush();
		}

		void CaptureDiagnosticReport(const std::string_view reason, const std::string_view detail)
		{
			if (!g_installed.load())
			{
				return;
			}

			std::scoped_lock lock(g_writeMutex);
			const auto basePath = BuildCrashBasePath();

			std::filesystem::path dumpPath;
#ifdef _WIN32
			auto dumpTarget = basePath;
			dumpTarget.replace_extension(".dmp");
			dumpPath = WriteMiniDump(dumpTarget, nullptr); // no exception context, but all-thread state still captured
#endif

			auto reportPath = basePath;
			reportPath.replace_extension(".txt");
			WriteTextCrashReport(reportPath, reason, detail, nullptr, 0, dumpPath);
		}

#ifdef _WIN32
		LONG WINAPI UnhandledExceptionFilterThunk(EXCEPTION_POINTERS* exceptionPointers)
		{
			CaptureCrashArtifacts("UnhandledException", "Structured exception captured.", exceptionPointers, 0);
			return EXCEPTION_EXECUTE_HANDLER;
		}
#endif

		[[noreturn]] void TerminateHandlerThunk()
		{
			std::string detail = "Unknown terminate cause.";
			if (const auto exceptionPtr = std::current_exception(); exceptionPtr != nullptr)
			{
				try
				{
					std::rethrow_exception(exceptionPtr);
				}
				catch (const std::exception& e)
				{
					detail = e.what();
				}
				catch (...)
				{
					detail = "Non-standard exception reached std::terminate.";
				}
			}

			CaptureCrashArtifacts("Terminate", detail, nullptr, 0);
			std::_Exit(3);
		}

		void SignalHandlerThunk(int signalNumber)
		{
			CaptureCrashArtifacts("Signal", "Signal handler invoked.", nullptr, signalNumber);
			std::_Exit(3);
		}
	} // namespace

	void CrashHandler::Install(std::string_view appName)
	{
		if (g_installed.exchange(true))
		{
			return;
		}

		g_appName = std::string(appName);

		std::set_terminate(TerminateHandlerThunk);
		std::signal(SIGABRT, SignalHandlerThunk);
		std::signal(SIGTERM, SignalHandlerThunk);

#ifndef _WIN32
		std::signal(SIGFPE, SignalHandlerThunk);
		std::signal(SIGILL, SignalHandlerThunk);
		std::signal(SIGSEGV, SignalHandlerThunk);
#endif

#ifdef _WIN32
		SetUnhandledExceptionFilter(UnhandledExceptionFilterThunk);
#endif

		AE_INFO(LogCategory::Engine, "Crash handler installed for application '{}'.", g_appName);
	}

	void CrashHandler::Uninstall()
	{
		if (!g_installed.exchange(false))
		{
			return;
		}

		std::signal(SIGABRT, SIG_DFL);
		std::signal(SIGTERM, SIG_DFL);

#ifndef _WIN32
		std::signal(SIGFPE, SIG_DFL);
		std::signal(SIGILL, SIG_DFL);
		std::signal(SIGSEGV, SIG_DFL);
#endif
	}

	void CrashHandler::ReportGraphicsFault(std::string_view stage, std::string_view detail)
	{
		const std::string reason = std::format("GraphicsFault:{}", stage);
		CaptureDiagnosticReport(reason, detail);
	}

	void CrashHandler::SetContext(std::string_view key, std::string_view value)
	{
		std::scoped_lock lock(g_contextMutex);
		if (value.empty())
		{
			g_context.erase(std::string(key));
			return;
		}
		g_context[std::string(key)] = std::string(value);
	}
} // namespace aether
