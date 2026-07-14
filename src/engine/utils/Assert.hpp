#pragma once

#include "utils/Expected.hpp"
#include "utils/Logger.hpp"

#ifdef __clang__
#	define AE_DELETE_MSG(msg) delete(msg)
#else
#	define AE_DELETE_MSG(msg) delete
#endif

namespace aether
{
	inline void DebugBreak()
	{
#ifdef _MSC_VER
		__debugbreak();
#elif defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
		__builtin_debugtrap();
#else
		__builtin_trap();
#endif
	}

	// AE_ASSERT: Debug-only invariant check. Compiles out in release.
#ifdef NDEBUG
#	define AE_ASSERT(expr, msg) ((void)0)
#else
#	define AE_ASSERT(expr, msg) do { if (!(expr)) { ::aether::Logger::ErrorAt(::aether::LogCategory::Engine, std::source_location::current(), "Assert failed: {}", msg); ::aether::DebugBreak(); Throw(AetherError::Engine(msg)); } } while(0)
#endif

// AE_ASSERT_ALWAYS: Invariant check that runs in all builds.
#define AE_ASSERT_ALWAYS(expr, msg) do { if (!(expr)) { ::aether::Logger::ErrorAt(::aether::LogCategory::Engine, std::source_location::current(), "Assert failed: {}", msg); ::aether::Logger::Flush(); ::aether::DebugBreak(); std::abort(); } } while(0)
} // namespace aether
