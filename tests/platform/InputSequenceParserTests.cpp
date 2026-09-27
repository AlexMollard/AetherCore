// Input::ParseInputSequence: the engine.play_input_sequence text format. Regression
// coverage for the press-vs-hold bug (see Input.cpp's ParseInputSequence comment):
// "press" used to be a plain synonym for "hold" - one down event that never released,
// so a second "press" of an already-down key produced no new edge and a stateful
// device (a toggle bound to a key) silently stopped responding after the first one.
// These assert the event SHAPE the parser must produce, not incidental defaults.
#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include "platform/Input.hpp"

using namespace aether;

TEST_CASE("ParseInputSequence: press schedules a real down-then-up pair, not a permanent hold")
{
	std::string error;
	const auto events = Input::ParseInputSequence("0.0 press g", error);
	CHECK(error.empty());
	REQUIRE(events.size() == 2);
	CHECK(events[0].time == doctest::Approx(0.0f));
	CHECK(events[0].down == true);
	CHECK(events[1].time == doctest::Approx(0.1f));
	CHECK(events[1].down == false);
}

TEST_CASE("ParseInputSequence: hold schedules only a down event and never auto-releases")
{
	std::string error;
	const auto events = Input::ParseInputSequence("0.0 hold g", error);
	CHECK(error.empty());
	REQUIRE(events.size() == 1);
	CHECK(events[0].down == true);
}

TEST_CASE("ParseInputSequence: tap produces the same real-edge shape as press")
{
	std::string error;
	const auto events = Input::ParseInputSequence("0.0 tap g", error);
	CHECK(error.empty());
	REQUIRE(events.size() == 2);
	CHECK(events[0].down == true);
	CHECK(events[1].down == false);
}

TEST_CASE("ParseInputSequence: two presses of the same key each get their own edge")
{
	// The literal reported bug: a discriminator sequence of two presses inside one
	// play_input_sequence invocation used to collapse to a single down event (the
	// second "press" re-set an already-down synthetic key, no new edge). Must now
	// yield two independent down/up pairs.
	std::string error;
	const auto events = Input::ParseInputSequence("0.0 press g\n1.0 press g", error);
	CHECK(error.empty());
	REQUIRE(events.size() == 4);
	CHECK(events[0].down == true);
	CHECK(events[0].time == doctest::Approx(0.0f));
	CHECK(events[1].down == false);
	CHECK(events[1].time == doctest::Approx(0.1f));
	CHECK(events[2].down == true);
	CHECK(events[2].time == doctest::Approx(1.0f));
	CHECK(events[3].down == false);
	CHECK(events[3].time == doctest::Approx(1.1f));
}

TEST_CASE("ParseInputSequence: unknown op is rejected rather than silently ignored")
{
	std::string error;
	const auto events = Input::ParseInputSequence("0.0 poke g", error);
	CHECK(events.empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE("ParseInputSequence: a malformed stick line is rejected, not read as a centred stick")
{
	for (const char* bad: {"0.0 stick 0.5", "0.0 stick 0.5 up", "0.0 stick 0.5x 1", "0.0 stick 1 2 3"})
	{
		std::string error;
		const auto events = Input::ParseInputSequence(bad, error);
		CHECK(events.empty());
		CHECK_FALSE(error.empty());
	}
}

// A sequence's stick line drives pad 0's left stick on the sequence clock, and 'clear' centres it again
// (a route leg that ends with 'clear' must not leave Crash running).
TEST_CASE("play_input_sequence: stick moves pad 0's left stick and clear centres it")
{
	std::string error;
	const auto events = Input::ParseInputSequence("0.0 stick 1 -2\n0.0 hold w", error);
	REQUIRE(error.empty());
	REQUIRE(events.size() == 2);
	Input input;
	input.PlayInputSequence(events);
	input.Update();
	REQUIRE(input.IsGamepadConnected());
	const glm::vec2 stick = input.GetGamepadStick(GamepadStick::Left);
	CHECK(stick.x > 0.6f);
	CHECK(stick.y > 0.6f); // raw -2 clamps to -1: pushed fully up, positive y
	CHECK(input.IsKeyDown(Key::W));

	input.PlayInputSequence(Input::ParseInputSequence("0.0 clear", error));
	input.Update();
	CHECK(glm::length(input.GetGamepadStick(GamepadStick::Left)) == doctest::Approx(0.0f));
	CHECK_FALSE(input.IsKeyDown(Key::W));
}
