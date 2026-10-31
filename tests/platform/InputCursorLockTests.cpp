// Cursor-lock request/focus gating, windowless: SetCursorLocked/ApplyCursorMode need a
// real GLFW window (no-op without one, exactly like SetOsCursorVisible already does), but
// UpdateCursorLock's decision logic - does a request actually become the applied lock - is
// pure state and fully exercisable headless. The Escape-consumption nuance needs a real key
// press/release across frames (Update() needs a window) and is covered by live proof
// instead; see the mouse-capture task's report.
#include <doctest/doctest.h>

#include "platform/Input.hpp"

using namespace aether;

namespace aether
{
	// Replays the native sampling math used by Input::Update without changing OS focus.
	struct InputFocusTestAccess
	{
		static void Sample(Input& input, glm::vec2 position, bool focused)
		{
			input.ApplyNativeMouseSample(position, focused);
		}
		static void Focus(Input& input, bool focused)
		{
			input.ApplyNativeFocusEvent(focused);
		}
	};
}

TEST_CASE("Native mouse sampling discards desktop movement on the first focused frame")
{
	Input input;
	InputFocusTestAccess::Sample(input, {10, 20}, true);
	CHECK(input.GetMouseDelta() == glm::vec2{});
	InputFocusTestAccess::Sample(input, {13, 18}, true);
	CHECK(input.GetMouseDelta() == glm::vec2(3, -2));

	bool requestedLock = false;
	SUBCASE("an unlocked cursor") {}
	SUBCASE("a cursor unlocked by focus loss")
	{
		requestedLock = true;
		input.RequestCursorLock(true);
		input.UpdateCursorLock(true);
		InputFocusTestAccess::Sample(input, {13, 18}, true);
		input.UpdateCursorLock(false);
		CHECK_FALSE(input.IsCursorLocked());
	}
	for (int frame = 0; frame < 3; ++frame)
	{
		InputFocusTestAccess::Sample(input, {1000 + float(frame), 2000}, false);
		CHECK(input.GetMousePos() == glm::vec2(13, 18));
		CHECK(input.GetMouseDelta() == glm::vec2{});
	}

	// Gameplay reads this sample before the application's UpdateCursorLock call.
	InputFocusTestAccess::Sample(input, {700, 800}, true);
	CHECK(input.GetMousePos() == glm::vec2(700, 800));
	CHECK(input.GetMouseDelta() == glm::vec2{});
	input.UpdateCursorLock(true);
	CHECK(input.IsCursorLocked() == requestedLock);
	if (requestedLock)
	{
		// Switching back into GLFW's virtual coordinate space needs its own resync.
		InputFocusTestAccess::Sample(input, {900, 900}, true);
		CHECK(input.GetMouseDelta() == glm::vec2{});
	}
	const glm::vec2 previous = input.GetMousePos();
	InputFocusTestAccess::Sample(input, previous + glm::vec2(4, -3), true);
	CHECK(input.GetMouseDelta() == glm::vec2(4, -3));
}

TEST_CASE("Native mouse sampling resyncs a focus cycle delivered within one event pump")
{
	Input input;
	InputFocusTestAccess::Sample(input, {10, 20}, true);
	CHECK(input.GetMouseDelta() == glm::vec2{});
	InputFocusTestAccess::Sample(input, {13, 18}, true);
	CHECK(input.GetMouseDelta() == glm::vec2(3, -2));

	bool requestedLock = false;
	SUBCASE("an unlocked cursor") {}
	SUBCASE("a requested cursor lock")
	{
		requestedLock = true;
		input.RequestCursorLock(true);
		input.UpdateCursorLock(true);
		InputFocusTestAccess::Sample(input, {13, 18}, true);
	}
	// The production GLFW callback forwards both events to this handler, but Update
	// may only see the final focused state after glfwPollEvents drains the queue.
	InputFocusTestAccess::Focus(input, false);
	InputFocusTestAccess::Focus(input, true);
	InputFocusTestAccess::Sample(input, {700, 800}, true);
	CHECK(input.GetMousePos() == glm::vec2(700, 800));
	CHECK(input.GetMouseDelta() == glm::vec2{});
	input.UpdateCursorLock(true);
	CHECK(input.IsCursorLocked() == requestedLock);
	InputFocusTestAccess::Sample(input, {704, 797}, true);
	CHECK(input.GetMouseDelta() == glm::vec2(4, -3));
}

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

TEST_CASE("An editor modal releases a captured cursor and owns gameplay input before polling")
{
	Input input;
	input.SetGameOwnsInput(true);
	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	input.SetOsCursorVisible(false);
	REQUIRE(input.IsCursorLocked());

	input.SetEditorModalInputBlocked(true);
	CHECK_FALSE(input.IsCursorLocked());
	CHECK_FALSE(input.GameOwnsInput());
	input.SetSyntheticKey(static_cast<int>(Key::Escape), true, false);
	input.SetSyntheticKey(static_cast<int>(Key::W), true, false);
	input.Update();
	input.UpdateCursorLock(true);
	CHECK_FALSE(input.IsCursorLocked());
	CHECK(input.IsOsCursorVisible());
	CHECK_FALSE(input.GameOwnsInput());
	CHECK_FALSE(input.IsKeyDown(Key::W));
	CHECK_FALSE(input.IsKeyPressed(Key::Escape));
	CHECK(static_cast<int>(input.GetKeyPressedThisFrame()) == -1);
}

TEST_CASE("Dismissing an editor modal requires a fresh viewport reclaim before locking")
{
	Input input;
	input.SetGameOwnsInput(true);
	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	input.SetEditorModalInputBlocked(true);
	input.SetEditorModalInputBlocked(false);
	input.RequestCursorLock(false);
	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	CHECK_FALSE(input.IsCursorLocked());
	input.SetGameOwnsInput(true);
	input.UpdateCursorLock(true);
	CHECK(input.IsCursorLocked());
}

TEST_CASE("A game Escape pause menu still receives the press without an editor modal")
{
	Input input;
	input.SetGameOwnsInput(true);
	input.RequestCursorLock(true);
	input.UpdateCursorLock(true);
	input.SetSyntheticKey(static_cast<int>(Key::Escape), true, false);
	input.Update();
	input.UpdateCursorLock(true);
	CHECK_FALSE(input.IsCursorLocked());
	CHECK(input.IsKeyPressed(Key::Escape));
}

TEST_CASE("A modal Escape remains quarantined while held after dismissal")
{
	Input input;
	input.SetEditorModalInputBlocked(true);
	input.SetSyntheticKey(static_cast<int>(Key::Escape), true, false);
	input.Update();
	input.SetEditorModalInputBlocked(false);
	input.Update();
	CHECK_FALSE(input.IsKeyDown(Key::Escape));
	input.ClearSyntheticKeys();
	input.Update();
	input.SetSyntheticKey(static_cast<int>(Key::Escape), true, false);
	input.Update();
	CHECK(input.IsKeyPressed(Key::Escape));
}

TEST_CASE("An editor modal suppresses mouse and gamepad actions")
{
	Input input;
	input.SetSyntheticMouseButton(0, true);
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadButton(0, GamepadButton::B, true);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftX, 1.0f);
	input.SetEditorModalInputBlocked(true);
	CHECK_FALSE(input.IsMouseButtonDown(MouseButton::Left));
	CHECK_FALSE(input.IsMouseButtonPressed(MouseButton::Left));
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::B));
	CHECK_FALSE(input.IsGamepadButtonPressed(GamepadButton::B));
	CHECK(input.GetGamepadStick(GamepadStick::Left) == glm::vec2{});
	CHECK(input.GetGamepadAxisRaw(GamepadAxis::LeftX) == 0.0f);
	CHECK(input.GetMouseDelta() == glm::vec2{});
	CHECK(input.IsGamepadConnected());
	CHECK(input.GetGamepadName() == "Synthetic Gamepad");
	input.SetGameOwnsInput(true);
	input.SetCursorLocked(true);
	input.SetOsCursorVisible(false);
	CHECK_FALSE(input.GameOwnsInput());
	CHECK_FALSE(input.IsCursorLocked());
	CHECK(input.IsOsCursorVisible());
}

TEST_CASE("A modal click cannot become a gameplay release after dismissal")
{
	Input input;
	input.SetEditorModalInputBlocked(true);
	input.SetSyntheticMouseButton(0, true);
	input.Update();
	input.SetEditorModalInputBlocked(false);
	input.Update();
	CHECK_FALSE(input.IsMouseButtonDown(MouseButton::Left));
	input.SetSyntheticMouseButton(0, false);
	input.Update();
	CHECK_FALSE(input.IsMouseButtonReleased(MouseButton::Left));
	input.Update();
	input.SetSyntheticMouseButton(0, true);
	input.Update();
	CHECK(input.IsMouseButtonPressed(MouseButton::Left));
}

TEST_CASE("A modal gamepad button cannot become a gameplay release after dismissal")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetEditorModalInputBlocked(true);
	input.SetSyntheticGamepadButton(0, GamepadButton::B, true);
	input.Update();
	input.SetEditorModalInputBlocked(false);
	input.Update();
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::B));
	input.SetSyntheticGamepadButton(0, GamepadButton::B, false);
	// The windowless gamepad injector seeds the current/previous button states.
	CHECK_FALSE(input.IsGamepadButtonReleased(GamepadButton::B));
	input.Update();
	input.SetSyntheticGamepadButton(0, GamepadButton::B, true);
	input.Update();
	CHECK(input.IsGamepadButtonDown(GamepadButton::B));
}
