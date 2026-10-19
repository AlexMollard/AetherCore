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
