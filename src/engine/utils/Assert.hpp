#pragma once

#include "utils/Expected.hpp"
#include "utils/Logger.hpp"

// AE_DELETE_MSG: Compiler-portable delete("reason") for C++26.
// Clang supports = delete("message") (P2518R2); MSVC does not yet.
// Use as: Foo(const Foo&) = AE_DELETE_MSG("reason");
// On Clang this expands to: Foo(const Foo&) = delete("reason");
// On MSVC this expands to: Foo(const Foo&) = delete;
#if defined(__clang__)
#	define AE_DELETE_MSG(msg) delete(msg)
#else
#	define AE_DELETE_MSG(msg) delete
#endif

namespace aether
{
	// AE_ASSERT: Debug-only invariant check. Compiles out in release.
	// Use for "this should never happen" conditions that are programmer bugs.
	// Will map to std::contract_assert when compilers ship C++26 Contracts.
#ifdef NDEBUG
#	define AE_ASSERT(expr, msg) ((void)0)
#else
#	define AE_ASSERT(expr, msg) do { if (!(expr)) { ::aether::Logger::ErrorAt(::aether::LogCategory::Engine, std::source_location::current(), "Assert failed: {}", msg); Throw(AetherError::Engine(msg)); } } while(0)
#endif

// AE_ASSERT_ALWAYS: Invariant check that runs in all builds.
// Use for conditions where continuing would cause undefined behavior
// (e.g., null device, stale handle, out-of-bounds index).
#define AE_ASSERT_ALWAYS(expr, msg) do { if (!(expr)) { ::aether::Logger::ErrorAt(::aether::LogCategory::Engine, std::source_location::current(), "Assert failed: {}", msg); Throw(AetherError::Engine(msg)); } } while(0)
} // namespace aether
