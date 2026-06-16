#include "utils/Backtrace.hpp"

#include <array>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#	include <DbgHelp.h>
#endif

namespace aether
{

	std::string_view ShortenPath(const char* path) noexcept
	{
		const std::string_view p(path);
		auto pos = p.rfind("src/");
		if (pos == std::string_view::npos)
		{
			pos = p.rfind("src\\");
		}
		if (pos != std::string_view::npos)
		{
			return p.substr(pos);
		}
		return p;
	}

	int CaptureBacktrace(void** buffer, int maxDepth, int skipFrames) noexcept
	{
#ifdef _WIN32
		// +1 to skip this CaptureBacktrace frame itself
		return CaptureStackBackTrace(static_cast<DWORD>(skipFrames + 1), static_cast<DWORD>(maxDepth), buffer, nullptr);
#else
		(void) buffer;
		(void) maxDepth;
		(void) skipFrames;
		return 0;
#endif
	}

	std::string ResolveAddress(void* addr) noexcept
	{
#ifdef _WIN32
		const auto address = reinterpret_cast<std::uint64_t>(addr);

		std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symBuf{};
		auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf.data());
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = MAX_SYM_NAME;

		DWORD64 displacement = 0;
		std::string result;
		if (SymFromAddr(GetCurrentProcess(), address, &displacement, sym) == TRUE)
		{
			result += sym->Name;
			if (displacement != 0)
			{
				result += "+0x";
				result += std::to_string(displacement);
			}
		}
		else
		{
			result += "0x" + std::to_string(address);
		}

		IMAGEHLP_LINE64 line{};
		line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
		DWORD lineDisplacement = 0;
		if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line) == TRUE)
		{
			result += " (";
			result += ShortenPath(line.FileName);
			result += ":";
			result += std::to_string(line.LineNumber);
			result += ")";
		}

		return result;
#else
		(void) addr;
		return {};
#endif
	}

} // namespace aether
