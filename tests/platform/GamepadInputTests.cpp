// Gamepad state, deadzones, and the two sign conventions that separate "GLFW reported
// something" from "a controller feels right".
//
// These run windowless: Update() needs a real GLFW window, so the synthetic setters seed
// live state directly, exactly as the synthetic keyboard and mouse already do for headless
// playtests. That covers everything except the polling loop itself - the one thing needing
// hardware - so the conversions below are testable without a controller plugged in, which
// is the only reason they are tested at all.
#include <doctest/doctest.h>

#include <cmath>
#include <glm/geometric.hpp>

#include "platform/Input.hpp"

using namespace aether;

// The regression that motivated seeding the axis array with GLFW's resting pose rather than
// zero. GLFW reports triggers on the same -1..+1 scale as sticks, so a zero-filled array is
// not "released" - it is dead centre, i.e. half pulled. A game reading RightTrigger as a
// throttle would drive off on its own.
TEST_CASE("An untouched trigger reads zero, not half pressed")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);

	CHECK(input.GetGamepadTrigger(GamepadTrigger::Left) == doctest::Approx(0.0f));
	CHECK(input.GetGamepadTrigger(GamepadTrigger::Right) == doctest::Approx(0.0f));
}

TEST_CASE("A fully pulled trigger reads one")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftTrigger, 1.0f);

	CHECK(input.GetGamepadTrigger(GamepadTrigger::Left) == doctest::Approx(1.0f));
	// The other trigger is untouched and must not have moved with it.
	CHECK(input.GetGamepadTrigger(GamepadTrigger::Right) == doctest::Approx(0.0f));
}

// GLFW follows SDL: pushing a stick up reports -1. This engine's world is y-up. Getting
// this wrong is not subtle - the character walks away from the stick - but it is invisible
// until someone holds a controller, which is exactly why it is pinned here.
TEST_CASE("Pushing the stick up gives a positive Y")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, -1.0f);

	CHECK(input.GetGamepadStick(GamepadStick::Left).y > 0.9f);
}

// The property a per-axis deadzone silently breaks: it leaves a SQUARE dead region, so a
// fully diagonal push reads (1, 1) - magnitude 1.41 - and the character moves 41% faster
// on the diagonals than on the cardinals.
TEST_CASE("A diagonal push is no faster than a straight one")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftX, 1.0f);
	const float cardinal = glm::length(input.GetGamepadStick(GamepadStick::Left));

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, -1.0f);
	const float diagonal = glm::length(input.GetGamepadStick(GamepadStick::Left));

	CHECK(cardinal == doctest::Approx(1.0f));
	CHECK(diagonal <= doctest::Approx(cardinal));
}

// Rescaling is what stops the stick jumping from nothing to a quarter-speed walk the
// instant it leaves the dead region.
TEST_CASE("Stick output ramps up from the deadzone edge rather than stepping")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetGamepadDeadzones(0.25f, 0.12f);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftX, 0.26f);

	const float magnitude = glm::length(input.GetGamepadStick(GamepadStick::Left));
	CHECK(magnitude > 0.0f);
	CHECK(magnitude < 0.05f);
}

TEST_CASE("A stick resting inside the deadzone reads exactly zero")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	// Well inside the 0.24 default - this is the drift a worn stick reports at rest.
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftX, 0.1f);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 0.1f);

	const glm::vec2 stick = input.GetGamepadStick(GamepadStick::Left);
	CHECK(stick.x == doctest::Approx(0.0f));
	CHECK(stick.y == doctest::Approx(0.0f));
}

// The reason every accessor defaults to kAnyGamepad. Controller slots are not packed, so a
// lone pad frequently is not in slot 0 - a dormant wireless receiver or a virtual device
// can hold it. Code that assumes slot 0 works on the machine it was written on and does
// nothing at all on the next one.
TEST_CASE("A controller in a slot other than zero is still found")
{
	Input input;
	input.SetSyntheticGamepadConnected(2, true);
	input.SetSyntheticGamepadButton(2, GamepadButton::A, true);

	CHECK(input.IsGamepadConnected());
	CHECK(input.ResolveGamepad(Input::kAnyGamepad) == 2);
	CHECK_FALSE(input.IsGamepadConnected(0));

	// Reached without the caller ever naming slot 2.
	CHECK(input.IsGamepadButtonDown(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::A, 0));
}

TEST_CASE("Button edges fire exactly once")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);

	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	CHECK(input.IsGamepadButtonPressed(GamepadButton::A));
	CHECK(input.IsGamepadButtonDown(GamepadButton::A));

	// Held through a second frame: still down, but no longer a fresh press.
	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	CHECK_FALSE(input.IsGamepadButtonPressed(GamepadButton::A));
	CHECK(input.IsGamepadButtonDown(GamepadButton::A));

	input.SetSyntheticGamepadButton(0, GamepadButton::A, false);
	CHECK(input.IsGamepadButtonReleased(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::A));
}

// Everything must read neutral with nothing plugged in, so a game can query the pad
// unconditionally without guarding every call. Note the trigger in particular: this is the
// same half-pressed bug as the first test, reached by a different route.
TEST_CASE("With no controller connected every read is neutral")
{
	const Input input;

	CHECK_FALSE(input.IsGamepadConnected());
	CHECK(input.ResolveGamepad(Input::kAnyGamepad) == -1);
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonPressed(GamepadButton::Start));
	CHECK(input.GetGamepadStick(GamepadStick::Left).x == doctest::Approx(0.0f));
	CHECK(input.GetGamepadStick(GamepadStick::Right).y == doctest::Approx(0.0f));
	CHECK(input.GetGamepadTrigger(GamepadTrigger::Left) == doctest::Approx(0.0f));
	CHECK(input.GetGamepadName().empty());
}

// Documented behaviour, asserted so it cannot drift into a phantom event: a pad that
// vanishes mid-hold stops reporting entirely rather than emitting a final release.
TEST_CASE("A disconnected pad stops reporting the button it was holding")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	REQUIRE(input.IsGamepadButtonDown(GamepadButton::A));

	input.SetSyntheticGamepadConnected(0, false);

	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonReleased(GamepadButton::A));
}

// The rescale divides by (1 - deadzone), so an unclamped 1.0 is a division by zero and the
// stick starts handing back infinities.
TEST_CASE("An absurd deadzone is clamped rather than dividing by zero")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetGamepadDeadzones(5.0f, 5.0f);

	CHECK(input.GetStickDeadzone() <= 0.9f);
	CHECK(input.GetTriggerDeadzone() <= 0.9f);

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftX, 1.0f);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftTrigger, 1.0f);
	CHECK(std::isfinite(input.GetGamepadStick(GamepadStick::Left).x));
	CHECK(std::isfinite(input.GetGamepadTrigger(GamepadTrigger::Left)));
}

TEST_CASE("Out-of-range slots are ignored rather than writing past the array")
{
	Input input;
	input.SetSyntheticGamepadConnected(99, true);
	input.SetSyntheticGamepadConnected(-1, true);
	input.SetSyntheticGamepadButton(99, GamepadButton::A, true);
	input.SetSyntheticGamepadAxis(-4, GamepadAxis::LeftX, 1.0f);

	CHECK_FALSE(input.IsGamepadConnected());
}

TEST_CASE("Clearing synthetic pads returns everything to disconnected")
{
	Input input;
	input.SetSyntheticGamepadConnected(1, true);
	input.SetSyntheticGamepadButton(1, GamepadButton::Start, true);
	REQUIRE(input.IsGamepadConnected());

	input.ClearSyntheticGamepads();

	CHECK_FALSE(input.IsGamepadConnected());
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::Start));
	CHECK(input.GetGamepadTrigger(GamepadTrigger::Right) == doctest::Approx(0.0f));
}

// ── Stick-as-direction ──────────────────────────────────────────────────────
// The property a menu depends on. A stick is a position, not an event, so "is it pushed
// down" is true every frame it is held - a choice list written against that scrolls to the
// bottom in a few frames. Each windowless SetSyntheticGamepadAxis call stands in for one
// frame's poll.

TEST_CASE("Holding the stick flicks once, not once per frame")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f); // raw +1 is down
	CHECK(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));

	// Still held, next frame - the menu must not step again.
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f);
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));
}

TEST_CASE("Returning the stick to centre re-arms the flick")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f);
	REQUIRE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 0.0f);
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f);
	CHECK(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));
}

// Why engagement and release use different thresholds. With one threshold, a stick held
// near the line crosses it repeatedly as it jitters and the menu walks on its own.
TEST_CASE("A stick easing back but staying past the release point does not re-flick")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f);
	REQUIRE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));

	// Both thresholds are compared against the DEADZONED magnitude, not the raw axis - the
	// raw value is not what any caller sees. Raw 0.56 lands at (0.56 - 0.24) / 0.76 = 0.42
	// once rescaled: below the 0.5 engage point, above the 0.35 release point, so still held.
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 0.56f);
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f);
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));
}

TEST_CASE("Pushing the stick one way does not flick the others")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftX, 1.0f);

	CHECK(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Right));
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Left));
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Up));
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Left, GamepadDirection::Down));
	// The other stick is untouched.
	CHECK_FALSE(input.IsGamepadStickFlicked(GamepadStick::Right, GamepadDirection::Right));
}

// ── Button consumption ──────────────────────────────────────────────────────
// The gamepad half of ConsumeKey. Without it the A that confirms a menu item also reaches
// the game and the character jumps as the menu closes.

TEST_CASE("A consumed button is invisible to everything downstream")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	REQUIRE(input.IsGamepadButtonPressed(GamepadButton::A));

	input.ConsumeGamepadButton(GamepadButton::A);

	CHECK(input.IsGamepadButtonConsumed(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonPressed(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::A));
	// Consuming one button says nothing about any other.
	CHECK_FALSE(input.IsGamepadButtonConsumed(GamepadButton::B));
}

TEST_CASE("Consumption lasts one frame and not longer")
{
	Input input;
	input.SetSyntheticGamepadConnected(0, true);
	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	input.ConsumeGamepadButton(GamepadButton::A);
	REQUIRE_FALSE(input.IsGamepadButtonDown(GamepadButton::A));

	// Next frame, still held: the consumption expired with the frame that made it.
	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	CHECK(input.IsGamepadButtonDown(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonConsumed(GamepadButton::A));
}
