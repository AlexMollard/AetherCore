// IsDebuggerAttached decides whether the editor hands off to a separate Launcher process
// or opens the launcher in place. Getting it stuck on "true" would quietly retire the
// multi-process launcher for everyone; stuck on "false" leaves the original problem, where
// switching projects under a debugger kills the session being stepped through.
//
// A test binary is normally run without a debugger, so that is what this asserts. To check
// the other direction, run this same case under one - it must flip:
//
//   cdb -g -c "g" EngineTests.exe --test-case="*debugger*"
//
// AETHER_EXPECT_DEBUGGER=1 makes that run assert the opposite, so the debugged case is a
// real assertion rather than something read off the console.
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "platform/CrashHandler.hpp"

TEST_CASE("IsDebuggerAttached reports whether this process is being debugged")
{
	const char* expectDebugger = std::getenv("AETHER_EXPECT_DEBUGGER");
	const bool expected = expectDebugger != nullptr && std::string(expectDebugger) == "1";

	INFO("AETHER_EXPECT_DEBUGGER=" << (expectDebugger != nullptr ? expectDebugger : "(unset)"));
	CHECK(aether::IsDebuggerAttached() == expected);
}

TEST_CASE("IsDebuggerAttached is stable across calls")
{
	// Queried per call rather than cached at startup, so attaching to a running process is
	// picked up. That must not make it flicker between two calls in the same instant.
	const bool first = aether::IsDebuggerAttached();
	CHECK(aether::IsDebuggerAttached() == first);
	CHECK(aether::IsDebuggerAttached() == first);
}
