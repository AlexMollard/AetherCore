#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "ui/UiWidgetSystem.hpp"

using namespace aether;

TEST_CASE("SliderNormalized clamps to 0..1")
{
	CHECK(ui::SliderNormalized(0.5f, 0.f, 1.f) == doctest::Approx(0.5f));
	CHECK(ui::SliderNormalized(-1.f, 0.f, 1.f) == doctest::Approx(0.f));
	CHECK(ui::SliderNormalized(5.f, 0.f, 10.f) == doctest::Approx(0.5f));
	CHECK(ui::SliderNormalized(2.f, 0.f, 0.f) == doctest::Approx(0.f)); // degenerate range
}

TEST_CASE("SliderQuantize snaps to the nearest step and clamps")
{
	CHECK(ui::SliderQuantize(0.52f, 0.f, 1.f, 0.05f) == doctest::Approx(0.5f));
	CHECK(ui::SliderQuantize(0.53f, 0.f, 1.f, 0.05f) == doctest::Approx(0.55f));
	CHECK(ui::SliderQuantize(1.4f, 0.f, 1.f, 0.05f) == doctest::Approx(1.f));   // clamp high
	CHECK(ui::SliderQuantize(-0.4f, 0.f, 1.f, 0.05f) == doctest::Approx(0.f));  // clamp low
	CHECK(ui::SliderQuantize(0.377f, 0.f, 1.f, 0.f) == doctest::Approx(0.377f)); // continuous
}

TEST_CASE("SliderValueFromMouseX maps track-relative x to a stepped value")
{
	const glm::vec4 track{100.f, 0.f, 204.f, 20.f}; // pad 2 -> innerX 102, innerW 200
	CHECK(ui::SliderValueFromMouseX(102.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(0.f));
	CHECK(ui::SliderValueFromMouseX(302.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(1.f));
	CHECK(ui::SliderValueFromMouseX(202.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(0.5f));
	CHECK(ui::SliderValueFromMouseX(50.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(0.f)); // left of track
}
