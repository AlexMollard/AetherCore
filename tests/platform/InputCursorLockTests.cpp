// Cursor-lock request/focus gating, windowless: SetCursorLocked/ApplyCursorMode need a
// real GLFW window (no-op without one, exactly like SetOsCursorVisible already does), but
// UpdateCursorLock's decision logic - does a request actually become the applied lock - is
// pure state and fully exercisable headless. The Escape-consumption nuance needs a real key
// press/release across frames (Update() needs a window) and is covered by live proof
// instead; see the mouse-capture task's report.
#include <doctest/doctest.h>

#include "platform/Input.hpp"

using namespace aether;

TEST_CASE("A game that never requests cursor lock is never locked")
{
	Input input;
	CHECK_FALSE(input.IsCursorLockRequested());

	input.UpdateCursorLock(/*windowFocused=*/true);
	CHECK_FALSE(input.IsCursorLocked());
}

TEST_CASE("Requesting cursor lock while focused actually locks")
{
	Input input;
	input.RequestCursorLock(true);
	CHECK(input.IsCursorLockRequested());

	input.UpdateCursorLock(/*windowFocused=*/true);
	CHECK(input.IsCursorLocked());
}

TEST_CASE("Cursor lock never applies while the window is unfocused")
{
	Input input;
	input.RequestCursorLock(true);

	input.UpdateCursorLock(/*windowFocused=*/false);
	CHECK_FALSE(input.IsCursorLocked());
}

TEST_CASE("Losing focus releases an active cursor lock, and regaining it locks again")
{
	Input input;
	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	REQUIRE(input.IsCursorLocked());

	input.UpdateCursorLock(false);
	CHECK_FALSE(input.IsCursorLocked());

	// Focus alone (not Escape) never latches the lock off - regaining it while the
	// request still stands locks right back up.
	input.UpdateCursorLock(true);
	CHECK(input.IsCursorLocked());
}

TEST_CASE("Dropping the request releases the lock, and reasserting it locks again")
{
	Input input;
	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	REQUIRE(input.IsCursorLocked());

	input.RequestCursorLock(false);
	input.UpdateCursorLock(true);
	CHECK_FALSE(input.IsCursorLocked());

	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	CHECK(input.IsCursorLocked());
}
