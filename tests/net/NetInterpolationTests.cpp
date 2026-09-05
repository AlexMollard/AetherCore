#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

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

// ── RecommendedDelaySeconds ───────────────────────────────────────────────────
//
// The fixed 0.1s `interpolationDelaySeconds` guess is right for SOME 20Hz link
// and wrong for every other one: too slow for a tight LAN connection, and not
// necessarily wide enough for a jittery one. These drive the buffer with real
// timings - not a mocked clock - and check the delay it derives from them.
// Every case here is new behaviour: none of it compiles against the pre-change
// InterpolationBuffer, which had no RecommendedDelaySeconds at all.

TEST_CASE("Before any interval is measured, the recommended delay is the anchor")
{
	net::InterpolationBuffer buf;
	// No samples at all: nothing has been observed yet.
	CHECK(buf.RecommendedDelaySeconds(0.1f) == doctest::Approx(0.1f));

	// One sample is a point, not an interval - still nothing to derive from.
	buf.Push({.time = 0.f, .position = {0.f, 0.f, 0.f}});
	CHECK(buf.RecommendedDelaySeconds(0.1f) == doctest::Approx(0.1f));

	// A different anchor is honoured live, proving this is a genuine fallback
	// and not a cached copy of the first value ever passed in.
	CHECK(buf.RecommendedDelaySeconds(0.25f) == doctest::Approx(0.25f));
}

TEST_CASE("A tight, steady link settles far below the fixed 100 ms guess")
{
	// A 100Hz link - tighter than the 20Hz the 0.1s anchor was sized for. Every
	// other player on a connection this good should render close to what just
	// arrived, not a tenth of a second behind it.
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 20; ++i)
	{
		t += 0.01f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}

	const float delay = buf.RecommendedDelaySeconds(0.1f);
	CHECK(delay > 0.f);
	CHECK(delay < 0.05f); // well under half the fixed guess
}

TEST_CASE("The recommended delay never collapses to zero even on a near-instant link")
{
	// Floored: an interpolation delay of (near) zero leaves no slack at all, so
	// the buffer starves the instant one packet is a fraction of a millisecond
	// late - which is the ordinary case, not the exceptional one.
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 30; ++i)
	{
		t += 0.0005f; // 2000 Hz - far tighter than any real send rate
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}

	const float delay = buf.RecommendedDelaySeconds(0.1f);
	CHECK(delay >= 0.015f); // a real floor, not merely "greater than zero"
	CHECK(delay < 0.05f);   // and still nowhere near the fixed 100 ms guess
}

TEST_CASE("A jittery link widens the delay and settles instead of chasing every spike")
{
	// Interval alternating 30ms/70ms - a real 20Hz-ish link with real jitter, not
	// the fixed spacing every other case in this file uses. The average interval
	// (50ms) alone would justify a delay near the old fixed guess; the point of
	// this case is that the MARGIN added for the jitter is bounded and settles.
	net::InterpolationBuffer buf;
	float t = 0.f;
	std::vector<float> recent;
	for (int i = 0; i < 40; ++i)
	{
		t += (i % 2 == 0) ? 0.03f : 0.07f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
		if (i >= 30)
		{
			recent.push_back(buf.RecommendedDelaySeconds(0.1f));
		}
	}

	// Wider than the anchor - the jitter margin is doing real work - but nowhere
	// near the ceiling: this is a real but ordinary link, not a pathological one.
	for (const float delay: recent)
	{
		CHECK(delay > 0.1f);
		CHECK(delay < 0.3f);
	}

	// Settled, not oscillating without bound: across the tail end, once the
	// smoothing has had time to warm up, consecutive values stay within a tight
	// band rather than swinging across the whole [0.1, 0.3] range checked above.
	const auto [minIt, maxIt] = std::minmax_element(recent.begin(), recent.end());
	CHECK(*maxIt - *minIt < 0.01f);
}

TEST_CASE("A pathological, wildly variable link is capped rather than growing without bound")
{
	// Interval alternating 10ms/600ms - loss-and-burst behaviour far outside
	// anything a jitter margin should try to fully cover. Ceilinged so this
	// cannot push the render delay somewhere absurd.
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 60; ++i)
	{
		t += (i % 2 == 0) ? 0.01f : 0.6f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}

	const float delay = buf.RecommendedDelaySeconds(0.1f);
	CHECK(delay <= 0.5f);
	CHECK(delay > 0.1f); // still wider than the anchor - this link really is bad
}

TEST_CASE("A stall does not poison the estimate, and recovery does not inherit it")
{
	// A steady 20Hz rhythm, then a multi-second gap - a paused game, a scene
	// load, or simply an idle entity that stopped changing for a while - then
	// the same rhythm resumes. The gap must not be read as "the link is now this
	// slow": that would spike the delay for a long time afterward at
	// kIntervalSmoothingAlpha's crawl rate.
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 20; ++i)
	{
		t += 0.05f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}
	const float beforeStall = buf.RecommendedDelaySeconds(0.1f);
	CHECK(beforeStall < 0.1f); // already settled tighter than the anchor

	t += 3.f; // the stall
	buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	// Nothing to measure the gap against yet - the arrival right after a stall is
	// treated like the first sample of a session, not like a 3-second interval.
	CHECK(buf.RecommendedDelaySeconds(0.1f) == doctest::Approx(0.1f));

	// The rhythm resumes: the estimate rebuilds from here, not from the stall.
	for (int i = 0; i < 20; ++i)
	{
		t += 0.05f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}
	const float afterStall = buf.RecommendedDelaySeconds(0.1f);
	CHECK(afterStall < 0.1f);
	CHECK(afterStall == doctest::Approx(beforeStall).epsilon(0.05));
}

TEST_CASE("Out-of-order and duplicate arrivals do not perturb the settled estimate")
{
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 20; ++i)
	{
		t += 0.01f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}
	const float settled = buf.RecommendedDelaySeconds(0.1f);

	// A stale packet, delivered late (UDP), naming an earlier point in time than
	// the newest arrival already on record.
	buf.Push({.time = t - 0.5f, .position = {-1.f, 0.f, 0.f}});
	CHECK(buf.RecommendedDelaySeconds(0.1f) == doctest::Approx(settled));

	// A duplicate of the newest timestamp - two channels landing on the same
	// tick, or the collision ResolveTransforms itself can produce.
	buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	CHECK(buf.RecommendedDelaySeconds(0.1f) == doctest::Approx(settled));
}
