#pragma once

#include <algorithm>
#include <cstddef>

namespace aether
{
	// Decides how much of a display interval to leave for work, so input can be latched as
	// late as possible before the frame that shows it.
	//
	// An earlier version of this idea was measured and REVERTED because it inferred the vsync
	// phase from the in-flight wait. That wait releases when a frame COMPLETES, which is not
	// the display flip, so pacing against it missed every other vsync: median frame time fell
	// below the interval with p95 at double it. This version is only safe because
	// PresentTimingTracker supplies a measured phase from VK_KHR_present_wait. If that
	// estimate is unavailable the caller must not pace at all - guessing is worse than
	// nothing, because a mistimed frame is a dropped frame and a dropped frame is visible.
	//
	// The reserve rises immediately when work grows or a deadline is missed and decays slowly
	// when there is room. That asymmetry is deliberate: being early costs latency nobody can
	// see in a frame-time graph, being late costs a hitch everybody can.
	//
	// Pure logic, no clock and no threads, so the policy is testable without timing luck.
	class LatencyPacer
	{
	public:
		// Never hand the whole interval to work; the OS needs slack to schedule us back in,
		// and sleep overshoot on Windows is routinely a millisecond.
		static constexpr float kFloorMs = 2.0f;

		// Multiplier on observed work. Work is measured after the fact, so the next frame is
		// always a prediction; this is the headroom for it being wrong.
		static constexpr float kSafety = 2.0f;

		// How fast the reserve gives ground once work settles. Deliberately slow, so a burst
		// of heavy frames keeps protecting later ones.
		static constexpr float kDecay = 0.99f;

		// The reserve may never claim the whole interval. Allowing it to reach the interval
		// LATCHES THE PACER OFF: the latch point becomes the previous flip, which is always in
		// the past, so no frame is ever paced - and an unpaced frame is more likely to miss,
		// which ratchets the reserve straight back up. Measured latched up at 16.30 ms against
		// a 16.44 ms period, with paced=1 in 2400 frames. Capping it leaves the controller its
		// full range of useful values and keeps the loop from closing on itself.
		static constexpr float kDefaultMaxReserveFraction = 0.5f;

		// workMs is everything between latching input and handing the frame off. missed says
		// the frame did not make the flip it was aimed at.
		void Observe(const float intervalMs, const float workMs, const bool missed)
		{
			if (!(intervalMs > 0.0f))
			{
				return;
			}

			const float target = workMs * kSafety;
			if (missed)
			{
				// Give up half the remaining interval at once, but never less than the work
				// actually demanded: a frame that overran by a lot needs the reserve pinned to
				// the interval, not nudged halfway to it.
				m_reserveMs = std::max({m_reserveMs, (m_reserveMs + intervalMs) * 0.5f, target});
			}
			else if (target > m_reserveMs)
			{
				m_reserveMs = target; // rise immediately
			}
			else
			{
				m_reserveMs = m_reserveMs * kDecay + target * (1.0f - kDecay); // decay slowly
			}

			// The floor cannot exceed the interval. A frame faster than kFloorMs is normal
			// before the flip estimate exists (the caller passes the loop period then), and
			// std::clamp with hi < lo is a debug assert - which wedged a Debug editor behind a
			// modal dialog on frame 0 - and undefined behaviour in release.
		const float ceilingMs = intervalMs * m_maxReserveFraction;
			m_reserveMs = std::clamp(m_reserveMs, std::min(kFloorMs, ceilingMs), ceilingMs);
			m_observations += 1;
		}

		// How long before the next flip to latch input. Zero until enough frames have been
		// observed to trust the estimate.
		[[nodiscard]] float ReserveMs() const
		{
			return m_observations < kWarmupFrames ? 0.0f : m_reserveMs;
		}

		// Set from graphics.latencyReserve so the trade between responsiveness and tail
		// length is the user's to make, not a constant baked in here.
		void SetMaxReserveFraction(float fraction)
		{
			m_maxReserveFraction = std::clamp(fraction, 0.1f, 0.9f);
		}

		[[nodiscard]] bool IsWarm() const
		{
			return m_observations >= kWarmupFrames;
		}

	private:
		// Long enough to see real work, short enough that startup is not spent uncontrolled.
		static constexpr std::size_t kWarmupFrames = 30;

		float m_reserveMs = kFloorMs;
		float m_maxReserveFraction = kDefaultMaxReserveFraction;
		std::size_t m_observations = 0;
	};
} // namespace aether
