// Input::ParseInputSequence: the engine.play_input_sequence text format. Regression
// coverage for the press-vs-hold bug (see Input.cpp's ParseInputSequence comment):
// "press" used to be a plain synonym for "hold" - one down event that never released,
// so a second "press" of an already-down key produced no new edge and a stateful
// device (a toggle bound to a key) silently stopped responding after the first one.
// These assert the event SHAPE the parser must produce, not incidental defaults.
#include <doctest/doctest.h>

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
