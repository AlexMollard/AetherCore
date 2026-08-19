#pragma once

#include <algorithm>
#include <cmath>

namespace aether
{
	// Snap the simulation delta onto the display's flip cadence.
	//
	// The delta handed to the simulation is the producer thread's own measured interval, but
	// a frame is not SEEN on the producer's schedule - it is seen when the display flips. On
	// a vsync-locked build those two clocks average out to the same rate while disagreeing
	// frame by frame, because the producer is released by an in-flight semaphore and a sleep
	// rather than by the flip itself.
	//
	// Measured on a 60 Hz display: flip period an even 16.85 ms with 5 missed flips in 600,
	// and a per-frame delta still ranging 14.53 - 19.95 ms. The simulation was perfectly
	// faithful to that delta - a body at constant velocity advanced at exactly 7.00 units/s
	// every single frame - and the motion still juddered, because the screen showed those
	// unequal advances at rigidly equal intervals. Faithfulness to the wrong clock is the
	// whole bug: nothing downstream can fix it, and interpolating the physics does not,
	// because the interpolation is driven by the same delta.
	//
	// So: when a delta is near a whole number of flip periods, use the flip period. It is
	// what the player will actually experience, and it is the number the frame is going to be
	// displayed for.
	//
	// Deliberately NOT a smoothing filter. An average lags a real change in frame rate and
	// keeps injecting error for several frames afterwards; snapping is exact when it applies
	// and does nothing at all when it does not, which is the behaviour worth having when the
	// alternative is quietly rewriting time.
	//
	// `periodSeconds` <= 0 means the cadence is unknown - no flips observed yet, vsync off, or
	// an uncapped frame rate - and the raw delta passes straight through.
	[[nodiscard]] inline double SnapDeltaToPresentCadence(const double rawSeconds, const double periodSeconds)
	{
		if (!(periodSeconds > 0.0) || !(rawSeconds > 0.0))
		{
			return rawSeconds;
		}

		const double periods = rawSeconds / periodSeconds;

		// Below about two thirds of a period there is no flip to snap to: the build is running
		// uncapped, several frames per flip, and rounding each of them up to a whole period
		// would multiply elapsed time by however many frames share that flip.
		if (periods < 0.5)
		{
			return rawSeconds;
		}

		const double nearest = std::round(periods);
		const double snapped = nearest * periodSeconds;

		// Only snap what is actually jitter. A frame that lands between multiples took time
		// nobody scheduled - a compile, a page fault, a driver stall - and it has to stay long,
		// or the world lurches to catch up on the frames after it.
		//
		// A third of a period is wide enough for the +-20% jitter measured above and still
		// narrower than the half-period gap between multiples, so a genuine hitch never
		// qualifies.
		constexpr double kSnapTolerance = 0.34;
		if (std::abs(periods - nearest) > kSnapTolerance)
		{
			return rawSeconds;
		}
		return snapped;
	}

	// Stateful wrapper around SnapDeltaToPresentCadence that carries the rounding error.
	//
	// Snapping alone drifts. The observed flip period is an ESTIMATE and is not exactly the
	// mean frame interval - measured here it sat at 16.993 ms while frames actually averaged
	// 16.700 - so every frame rounds the same way and the error compounds. That run gained
	// 1.9%: a second per minute, which pulls physics, timers and animation off the wall clock.
	//
	// The bias is far smaller than one quantum, so no stateless rule can see it. Carrying the
	// residual can: whatever a frame was given beyond its true length is owed back, and once
	// the debt reaches half a period the next frame rounds the other way and settles it. The
	// deltas stay quantised onto the cadence and the long-run total stays honest.
	class DeltaCadenceSnapper
	{
	public:
		[[nodiscard]] double Snap(const double rawSeconds, const double periodSeconds)
		{
			const double owed = rawSeconds + m_residual;
			const double snapped = SnapDeltaToPresentCadence(owed, periodSeconds);
			if (snapped == owed)
			{
				// Nothing was quantised - a hitch, or no cadence to snap to. Leave the residual
				// exactly as it was rather than folding a stall into it, or one long frame would
				// be repaid out of the frames after it and turn a single hitch into a wobble.
				return rawSeconds;
			}
			m_residual = owed - snapped;
			// A pathological period estimate must not let the debt run away.
			m_residual = std::clamp(m_residual, -kMaxResidualSeconds, kMaxResidualSeconds);
			return snapped;
		}

		void Reset()
		{
			m_residual = 0.0;
		}

		[[nodiscard]] double ResidualSeconds() const
		{
			return m_residual;
		}

	private:
		// Two frames at 30 Hz. Enough to absorb ordinary rounding, small enough that a bad
		// estimate cannot bank a visible amount of time and pay it out in one lump.
		static constexpr double kMaxResidualSeconds = 1.0 / 15.0;

		double m_residual = 0.0;
	};
} // namespace aether
