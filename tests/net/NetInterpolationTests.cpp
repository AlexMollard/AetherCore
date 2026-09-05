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

// ── SampleForward: dead reckoning past the newest sample ─────────────────────
//
// None of this compiles against the pre-change InterpolationBuffer, which had no
// SampleForward, no velocity trend, and no correction offset at all - every case
// below is new behaviour, and every one of them fails to link before this change.

TEST_CASE("SampleForward matches Sample() with fewer than two samples or no trend yet")
{
	net::InterpolationBuffer empty;
	CHECK_FALSE(empty.SampleForward(0.f, 0.2f).has_value());

	net::InterpolationBuffer one;
	one.Push({.time = 1.f, .position = {5.f, 0.f, 0.f}});
	const auto sample = one.SampleForward(10.f, 1.f);
	REQUIRE(sample.has_value());
	// Nothing to derive a velocity from: frozen at the single sample, exactly
	// like Sample() already holds.
	CHECK(sample->position.x == doctest::Approx(5.f));
}

TEST_CASE("SampleForward projects a steady mover forward using its established velocity")
{
	// A steady 20 units/second mover: three samples 50ms apart, each moving
	// exactly 1 unit - the same cadence a 20Hz send rate produces.
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.00f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 0.05f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 0.10f, .position = {2.f, 0.f, 0.f}});

	// 20ms past the newest sample, well inside a generous cap: the projection
	// continues the established trend.
	const auto forward = buf.SampleForward(0.12f, 0.15f);
	REQUIRE(forward.has_value());
	CHECK(forward->position.x == doctest::Approx(2.4f).epsilon(0.005));

	// Below the newest sample this must be identical to Sample() -
	// extrapolation only ever applies PAST the buffer's own data.
	const auto within = buf.Sample(0.075f);
	const auto withinForward = buf.SampleForward(0.075f, 0.15f);
	REQUIRE(within.has_value());
	REQUIRE(withinForward.has_value());
	CHECK(withinForward->position.x == doctest::Approx(within->position.x));
}

TEST_CASE("SampleForward freezes at the cap instead of running away when nothing more arrives")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.00f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 0.05f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 0.10f, .position = {2.f, 0.f, 0.f}}); // 20 units/s established

	constexpr float kCap = 0.1f;

	// Right at the cap: the full projection (2 + 20 * 0.1).
	const auto atCap = buf.SampleForward(0.10f + kCap, kCap);
	REQUIRE(atCap.has_value());
	CHECK(atCap->position.x == doctest::Approx(4.f));

	// A full second later - the sender may as well have stopped forever - the
	// entity must NOT have kept sliding at 20 units/s (that would place it
	// near 24) and must not have snapped back to the raw last sample (2)
	// either: it holds exactly where the projection reached at the cap.
	const auto wayPast = buf.SampleForward(0.10f + 1.0f, kCap);
	REQUIRE(wayPast.has_value());
	CHECK(wayPast->position.x == doctest::Approx(4.f));
}

TEST_CASE("A direction reversal is blended out over the correction window instead of snapped")
{
	net::InterpolationBuffer buf;
	// A steady mover long enough for the velocity trend to settle - see
	// UpdateMotion.
	buf.Push({.time = 0.00f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 0.05f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 0.10f, .position = {2.f, 0.f, 0.f}}); // 20 units/s established

	// The mover slams into reverse: the next real sample lands far from where
	// the established trend would have placed it.
	buf.Push({.time = 0.15f, .position = {-3.f, 0.f, 0.f}});

	// The instant this landed, the trend WOULD have projected the mover to
	// x=3 (2 + 20*0.05) - what a viewer was actually seeing a moment before
	// this packet arrived. Querying at exactly this instant must reproduce
	// THAT value, not jump straight to the raw new sample (-3): the guess is
	// corrected, not snapped.
	const auto atCorrection = buf.SampleForward(0.15f, 0.5f);
	REQUIRE(atCorrection.has_value());
	CHECK(atCorrection->position.x == doctest::Approx(3.f).epsilon(0.01));

	// Once the correction window has fully elapsed, the blend is completely
	// gone: this reads as a clean projection of the NEW (reversed) trend, with
	// no residual pull toward the old guess. New velocity is
	// (-3 - 2) / 0.05 = -100 units/s; `ahead` is capped at the window itself.
	constexpr float kCorrectionWindow = 0.15f; // mirrors InterpolationBuffer's own constant
	constexpr float kCap = 0.5f;
	const auto afterWindow = buf.SampleForward(0.15f + kCorrectionWindow, kCap);
	REQUIRE(afterWindow.has_value());
	CHECK(afterWindow->position.x == doctest::Approx(-3.f - 100.f * kCorrectionWindow).epsilon(0.01));

	// Midway through the window the result sits strictly between the two -
	// smoothly blending, never oscillating past either end.
	const auto midWindow = buf.SampleForward(0.15f + kCorrectionWindow * 0.5f, kCap);
	REQUIRE(midWindow.has_value());
	CHECK(midWindow->position.x < atCorrection->position.x);
	CHECK(midWindow->position.x > afterWindow->position.x);
}

TEST_CASE("An out-of-order arrival does not perturb the established velocity trend")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.00f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 0.05f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 0.10f, .position = {2.f, 0.f, 0.f}}); // 20 units/s established

	const auto before = buf.SampleForward(0.15f, 0.5f);
	REQUIRE(before.has_value());

	// A stale packet, delivered late (UDP), landing BETWEEN two samples
	// already on record and naming a wildly different position - so any
	// perturbation of the trend would be obvious.
	buf.Push({.time = 0.07f, .position = {999.f, 0.f, 0.f}});

	const auto after = buf.SampleForward(0.15f, 0.5f);
	REQUIRE(after.has_value());
	CHECK(after->position.x == doctest::Approx(before->position.x));
}

TEST_CASE("A stall forgets the velocity trend, and the resumed stream builds a fresh one")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.00f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 0.05f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 0.10f, .position = {2.f, 0.f, 0.f}}); // 20 units/s established

	// A multi-second gap - well past kStallAbsoluteSeconds - then one arrival.
	// If the OLD trend survived this, SampleForward just past it would keep
	// running at 20 units/s from x=500.
	buf.Push({.time = 5.00f, .position = {500.f, 0.f, 0.f}});
	const auto justAfterStall = buf.SampleForward(5.02f, 0.2f);
	REQUIRE(justAfterStall.has_value());
	// No trend yet: this arrival is the new baseline point, exactly like the
	// very first sample of a session, so it holds exactly like Sample() would.
	CHECK(justAfterStall->position.x == doctest::Approx(500.f));

	// A second post-stall arrival re-establishes a trend, built from ONLY
	// these two points rather than the forgotten pre-stall velocity.
	buf.Push({.time = 5.05f, .position = {505.f, 0.f, 0.f}}); // 100 units/s
	const auto rebuilt = buf.SampleForward(5.05f + 0.02f, 0.2f);
	REQUIRE(rebuilt.has_value());
	CHECK(rebuilt->position.x == doctest::Approx(505.f + 100.f * 0.02f));
}

// Mirrors NetworkReceiveSystem::ResolveTransforms' own formula directly against
// the buffer - ResolveTransforms reads the real wall clock and cannot be driven
// with known timings in a test. This is the one that actually matters: it is not
// enough for extrapolation to survive a late packet, it must also be SPENT so the
// render time genuinely moves closer to the present on an otherwise healthy link.
TEST_CASE("Spending an extrapolation budget moves the render time past data a full delay would still interpolate")
{
	net::InterpolationBuffer buf;
	buf.Push({.time = 0.00f, .position = {0.f, 0.f, 0.f}});
	buf.Push({.time = 0.05f, .position = {1.f, 0.f, 0.f}});
	buf.Push({.time = 0.10f, .position = {2.f, 0.f, 0.f}}); // 20 units/s established

	constexpr float kNow = 0.18f;
	constexpr float kDelay = 0.09f;  // a settled real-link delay
	constexpr float kBudget = 0.02f; // a representative fixed budget (see the pinned-budget escape)
	constexpr float kCap = 0.15f;

	// The FULL delay lands strictly inside the buffer - real interpolated data,
	// no guess involved at all. This is what every render used before this batch.
	const auto full = buf.Sample(kNow - kDelay);
	REQUIRE(full.has_value());
	CHECK(full->position.x == doctest::Approx(1.8f)); // interpolated between the last two samples

	// effectiveDelay = delay - min(delay, budget), exactly ResolveTransforms' own
	// formula. Spending the budget shrinks the delay enough to move renderTime
	// PAST the newest sample: the same query now needs SampleForward's forward
	// projection instead of Sample()'s interpolation, proving the effective
	// render time genuinely moved closer to the present - not merely that a late
	// packet is now survivable without moving it at all.
	const float effectiveDelay = kDelay - std::min(kDelay, kBudget);
	const auto budgeted = buf.SampleForward(kNow - effectiveDelay, kCap);
	REQUIRE(budgeted.has_value());
	CHECK(budgeted->position.x == doctest::Approx(2.2f)); // projected PAST the newest sample (2.f)
	CHECK(budgeted->position.x > full->position.x);
}

// MeasuredIntervalSeconds() and the self-tuning `autoExtrapolationBudget` shape
// it feeds (NetworkTransform::extrapolationBudgetFraction, default 1.0 - spend
// ALL of the measured interval, none of the jitter margin). None of this
// compiles against the pre-change InterpolationBuffer, which had no
// MeasuredIntervalSeconds at all.

TEST_CASE("Spending the full measured interval collapses the effective delay to (near) nothing on a clean link")
{
	// A steady, essentially zero-jitter link: interval and delay settle to
	// (almost) the same value, so spending the FULL measured interval as the
	// budget - the default fraction of 1.0 - leaves (almost) no effective delay:
	// exactly what a perfect link should be able to render at.
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 30; ++i)
	{
		t += 0.05f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}

	const float interval = buf.MeasuredIntervalSeconds();
	REQUIRE(interval == doctest::Approx(0.05f).epsilon(0.02));

	const float delay = buf.RecommendedDelaySeconds(0.1f);
	const float budget = std::clamp(interval * 1.0f, 0.f, 0.15f); // fraction=1.0, cap=0.15
	const float effectiveDelay = delay - std::min(delay, budget);
	CHECK(effectiveDelay < 0.01f);
}

TEST_CASE("Spending only the measured interval leaves a jittery link's own margin untouched")
{
	// The same 30ms/70ms alternating link the jitter-margin tests above drive:
	// a real, non-trivial jitter component is baked into `delay`. Spending only
	// the INTERVAL portion as the budget must leave a real, positive remainder -
	// the margin RecommendedDelaySeconds added specifically to protect against a
	// late packet must still be there, not spent away too.
	net::InterpolationBuffer buf;
	float t = 0.f;
	for (int i = 0; i < 40; ++i)
	{
		t += (i % 2 == 0) ? 0.03f : 0.07f;
		buf.Push({.time = t, .position = {t, 0.f, 0.f}});
	}

	const float interval = buf.MeasuredIntervalSeconds();
	const float delay = buf.RecommendedDelaySeconds(0.1f);
	const float budget = std::clamp(interval * 1.0f, 0.f, 0.15f);
	const float effectiveDelay = delay - std::min(delay, budget);

	CHECK(effectiveDelay > 0.02f);      // a real, protected margin remains
	CHECK(effectiveDelay < delay);      // strictly less than spending nothing at all
	CHECK(budget == doctest::Approx(interval)); // the WHOLE interval was spent, at fraction=1.0
}
