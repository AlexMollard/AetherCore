#include <doctest/doctest.h>

#include "net/NetInterpolation.hpp"

using namespace aether;

TEST_CASE("Sampling between two snapshots interpolates position")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 1.f, .position = {10.f, 0.f, 0.f}});

	const auto mid = buf.Sample(0.5f);
	REQUIRE(mid.has_value());
	CHECK(mid->position.x == doctest::Approx(5.f));

	const auto quarter = buf.Sample(0.25f);
	REQUIRE(quarter.has_value());
	CHECK(quarter->position.x == doctest::Approx(2.5f));
}

TEST_CASE("Sampling outside the buffer holds the end rather than extrapolating")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 1.f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 2.f, .position = {2.f, 0.f, 0.f}});

	const auto before = buf.Sample(0.f);
	REQUIRE(before.has_value());
	CHECK(before->position.x == doctest::Approx(1.f));

	const auto after = buf.Sample(99.f);
	REQUIRE(after.has_value());
	CHECK(after->position.x == doctest::Approx(2.f));
}

TEST_CASE("An empty buffer yields nothing")
{
	net::InterpolationBuffer buf;
	CHECK_FALSE(buf.Sample(0.f).has_value());
}

TEST_CASE("Out-of-order samples are stored in time order")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 2.f, .position = {20.f, 0.f, 0.f}});
	buf.Push({.time = 1.f, .position = {10.f, 0.f, 0.f}}); // arrived late

	const auto mid = buf.Sample(1.5f);
	REQUIRE(mid.has_value());
	CHECK(mid->position.x == doctest::Approx(15.f));
}

TEST_CASE("The buffer discards samples far older than the render window")
{
	net::InterpolationBuffer buf;
	for (int i = 0; i < 200; ++i)
	{
		buf.Push({.time = static_cast<float>(i) * 0.05f, .position = {static_cast<float>(i), 0.f, 0.f}});
	}
	// Unbounded growth over a long session is a leak; the buffer keeps a bounded window.
	CHECK(buf.Size() <= 64);

	// ...and it must keep the NEWEST samples. A trim from the wrong end would still
	// leave 64 entries and pass the size check above while silently discarding exactly
	// the samples the renderer needs, so assert the recent end survived and the ancient
	// one did not.
	const auto recent = buf.Sample(199.f * 0.05f);
	REQUIRE(recent.has_value());
	CHECK(recent->position.x == doctest::Approx(199.f));

	// Sampling before the retained window holds the oldest SURVIVING sample, which must
	// be far newer than sample 0 - if the old end had been kept this would be near 0.
	const auto oldest = buf.Sample(0.f);
	REQUIRE(oldest.has_value());
	CHECK(oldest->position.x > 100.f);
}

// Two samples can carry the SAME timestamp: NetworkReceiveSystem::ResolveTransforms
// captures `now` once per call, and a push is keyed to that value, so any two pushes a
// receiver makes inside one clock tick collide. lower_bound inserts at the FIRST element
// not less than the key, which puts the newer sample BEFORE the older one - and Sample()
// clamps to back(), so the entity renders the stale value and, once the sender stops
// moving and no further delta ever arrives, stays there permanently.
TEST_CASE("A sample sharing a timestamp with the newest still becomes the newest")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 1.0f, .position = {99.f, 0.f, 0.f}});
	buf.Push({.time = 1.0f, .position = {100.f, 0.f, 0.f}});

	// Render time is past both, so this is the clamp-to-newest path.
	const auto sample = buf.Sample(5.f);
	REQUIRE(sample.has_value());
	CHECK(sample->position.x == doctest::Approx(100.f));
}
