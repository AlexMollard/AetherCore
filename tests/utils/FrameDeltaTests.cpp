// The simulation delta is the producer thread's measured interval, but frames reach the
// screen on the display's cadence, not the producer's. Those are not the same clock, and
// where they disagree an object moving at a constant speed advances by a different amount
// in each equal slice of real time - which is exactly what "stuttery" looks like.
//
// Measured on a 60 Hz display with an even 16.85 ms flip period and 5 missed flips in 600,
// the per-frame delta still ranged 14.53 - 19.95 ms. The simulation was faithful to that
// delta (position advanced at exactly 7.00 units/s every frame) and the result still
// juddered, because the screen showed those unequal steps at equal intervals.
#include <doctest/doctest.h>

#include "utils/FrameDelta.hpp"

using namespace aether;

TEST_CASE("A delta close to the flip period is snapped onto it")
{
	constexpr double period = 0.01685;

	// The real spread from the measurement above.
	CHECK(SnapDeltaToPresentCadence(0.01453, period) == doctest::Approx(period));
	CHECK(SnapDeltaToPresentCadence(0.01995, period) == doctest::Approx(period));
	CHECK(SnapDeltaToPresentCadence(0.01685, period) == doctest::Approx(period));
}

TEST_CASE("A delta near a multiple of the flip period snaps to that multiple")
{
	constexpr double period = 0.01685;

	// A frame that misses one flip is genuinely two periods long, and must stay that way -
	// rounding it down to one would make the game run slow whenever it drops frames.
	CHECK(SnapDeltaToPresentCadence(0.0330, period) == doctest::Approx(period * 2.0));
	CHECK(SnapDeltaToPresentCadence(0.0500, period) == doctest::Approx(period * 3.0));
}

// The point of snapping is to remove jitter, NOT to hide hitches. A frame that took half
// again as long as any whole number of periods is a real stall, and pretending otherwise
// would make the world lurch to catch up afterwards.
TEST_CASE("A delta that is not near any multiple is left alone")
{
	constexpr double period = 0.01685;

	// 1.48 and 2.52 periods - as far from a flip boundary as a delta can get.
	CHECK(SnapDeltaToPresentCadence(0.0250, period) == doctest::Approx(0.0250));
	CHECK(SnapDeltaToPresentCadence(0.0425, period) == doctest::Approx(0.0425));
}

// The converse, and the reason the rule is "near a multiple" rather than "small": a long
// frame that does land on a multiple is snapped like any other, because it really will be
// displayed for that many flips. In practice the caller clamps the delta well before this
// matters - nothing above two periods reaches here - but the rule should not depend on that.
TEST_CASE("A long delta that lands on a multiple is still snapped")
{
	constexpr double period = 0.01685;

	// 5.93 periods: within a third of six, so it becomes exactly six.
	CHECK(SnapDeltaToPresentCadence(0.1000, period) == doctest::Approx(period * 6.0));
}

TEST_CASE("Snapping is off when the display cadence is unknown")
{
	// No flips observed yet, vsync disabled, or an unlocked frame rate: there is no cadence
	// to snap to, and inventing one would be worse than the jitter.
	CHECK(SnapDeltaToPresentCadence(0.0143, 0.0) == doctest::Approx(0.0143));
	CHECK(SnapDeltaToPresentCadence(0.0143, -1.0) == doctest::Approx(0.0143));
}

// An uncapped frame rate runs many frames per flip. Snapping those up to a whole period
// would multiply elapsed time several times over.
TEST_CASE("A delta far below one period is left alone")
{
	constexpr double period = 0.01685;

	CHECK(SnapDeltaToPresentCadence(0.0020, period) == doctest::Approx(0.0020));
	CHECK(SnapDeltaToPresentCadence(0.0060, period) == doctest::Approx(0.0060));
}

TEST_CASE("A zero or negative delta is returned unchanged")
{
	constexpr double period = 0.01685;

	CHECK(SnapDeltaToPresentCadence(0.0, period) == doctest::Approx(0.0));
	CHECK(SnapDeltaToPresentCadence(-0.5, period) == doctest::Approx(-0.5));
}

// The property that matters over a run: snapping must not make the clock drift. Jitter is
// symmetric about the flip period, so the snapped total has to track the real total rather
// than creeping ahead or behind.
TEST_CASE("Snapping a jittery sequence preserves total elapsed time")
{
	constexpr double period = 0.01685;
	const double samples[] = {0.01725, 0.01595, 0.01547, 0.01995, 0.01495, 0.01569, 0.01721,
	        0.01659, 0.01834, 0.01453, 0.01882, 0.01830, 0.01459, 0.01663, 0.01528, 0.01608};

	double raw = 0.0;
	double snapped = 0.0;
	for (const double sample: samples)
	{
		raw += sample;
		snapped += SnapDeltaToPresentCadence(sample, period);
	}

	// Within one flip period over sixteen frames.
	CHECK(snapped == doctest::Approx(raw).epsilon(0.05));
}

// The case the stateless version got wrong, measured in a real run: the observed flip period
// is not exactly the mean frame interval - the estimate sat at 16.99 ms while frames actually
// averaged 16.70 - so every frame rounded the same way and the world ran 1.9% fast. A second
// of drift per minute desynchronises physics, timers and animation from the wall clock.
//
// A stateless round cannot fix this, because the bias is smaller than one quantum: it needs
// to remember what it gave away and take it back.
TEST_CASE("A biased period estimate does not make the clock drift")
{
	constexpr double period = 0.016993; // the estimate observed in the run
	const double samples[] = {0.01725, 0.01595, 0.01547, 0.01995, 0.01495, 0.01569, 0.01721,
	        0.01659, 0.01834, 0.01453, 0.01882, 0.01830, 0.01459, 0.01663, 0.01528, 0.01608,
	        0.01670, 0.01640, 0.01700, 0.01620, 0.01690, 0.01655, 0.01710, 0.01635};

	DeltaCadenceSnapper snapper;
	double raw = 0.0;
	double snapped = 0.0;
	for (int repeat = 0; repeat < 20; ++repeat)
	{
		for (const double sample: samples)
		{
			raw += sample;
			snapped += snapper.Snap(sample, period);
		}
	}

	// Under half a percent over 480 frames, against 1.9% before the residual was carried.
	CHECK(snapped == doctest::Approx(raw).epsilon(0.005));
}

TEST_CASE("The snapper still quantises onto the cadence")
{
	constexpr double period = 0.01685;
	DeltaCadenceSnapper snapper;

	// Individual frames still land on the beat - that is the whole point; the residual only
	// decides which multiple, never whether to quantise at all.
	const double first = snapper.Snap(0.01453, period);
	CHECK(first == doctest::Approx(period));
}

TEST_CASE("A hitch is passed through and does not poison the residual")
{
	constexpr double period = 0.01685;
	DeltaCadenceSnapper snapper;

	CHECK(snapper.Snap(0.0250, period) == doctest::Approx(0.0250));
	// The frame after a pass-through is still snapped normally.
	CHECK(snapper.Snap(0.01453, period) == doctest::Approx(period));
}
