// GetKeyPressedThisFrame: the primitive a "press any key to rebind" prompt actually
// needs, added because the only alternative was a per-frame IsKeyPressed scan over
// every Key enum value from script - working, but kMaxKeys FFI calls a frame while a
// prompt is open. Runs windowless, exactly like GamepadInputTests/InputCursorLockTests:
// SetSyntheticKey seeds m_currKeys directly since there is no GLFW window to poll.
#include <doctest/doctest.h>

#include "platform/Input.hpp"

using namespace aether;

TEST_CASE("GetKeyPressedThisFrame reports Key.None when nothing was pressed")
{
	const Input input;
	CHECK(input.GetKeyPressedThisFrame() == static_cast<Key>(-1));
}

TEST_CASE("GetKeyPressedThisFrame reports the key that just transitioned down")
{
	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::A), true);
	CHECK(input.GetKeyPressedThisFrame() == Key::A);
}

TEST_CASE("GetKeyPressedThisFrame does not report a key already held from a previous frame")
{
	// The whole point of "pressed" rather than "down": a key held from before the
	// rebind prompt opened must not immediately satisfy it.
	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::A), true);
	input.Update(); // no window: rolls m_currKeys into m_prevKeys, matching a real frame boundary
	CHECK(input.GetKeyPressedThisFrame() == static_cast<Key>(-1));
}

TEST_CASE("GetKeyPressedThisFrame ignores a key already consumed by a higher-priority listener this frame")
{
	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::Escape), true);
	input.ConsumeKey(Key::Escape);
	CHECK(input.GetKeyPressedThisFrame() == static_cast<Key>(-1));
}
