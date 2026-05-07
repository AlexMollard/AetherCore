#pragma once

#include <chrono>
#include <thread>

#include "Profiler.hpp"

namespace aether
{
	// Regulates game-thread cadence to a fixed target frame rate.
	//
	// Usage (game loop):
	//
	//   m_framePacer.SetTargetFps(60.0f);
	//   while (running)
	//   {
	//       m_framePacer.Wait();                               // top of loop - sleep/spin to deadline
	//       const auto now = Clock::now();
	//       const double dt = duration<double>(now - prev).count(); prev = now;
	//       ... sim, PrepareFrame, SubmitFrame ...
	//   }
	//
	// Placing Wait() at the TOP of the loop before the deltaTime measurement gives
	// two benefits:
	//   1. deltaTime reflects the real inter-frame interval (including the sleep),
	//      so physics/animation integrate at the correct rate.
	//   2. SubmitFrame() no longer stalls: because the game thread waited ~targetDuration
	//      at the top of the loop, the render thread has had a full frame budget to finish
	//      the previous submission before it is asked to accept the next one.
	//
	// If a frame runs over-budget (GPU or CPU spike) the pacer clamps debt to one frame
	// - it will not try to compensate with a burst of back-to-back fast frames.
	//
	// Pass fps = 0 (the default) to run uncapped.
	class FramePacer
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;
		using Duration = Clock::duration; // nanoseconds - same rep as time_point arithmetic

		// Set target frame rate. Call before the game loop or at any time.
		// fps <= 0 disables pacing (uncapped).
		void SetTargetFps(float fps)
		{
			if (fps > 0.0f)
				m_targetDuration = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
			else
				m_targetDuration = Duration{ 0 };
			// Reset so the first Wait() after a rate change re-anchors the deadline
			// rather than trying to hit a deadline that may now be in the distant past.
			m_initialised = false;
		}

		[[nodiscard]] float GetTargetFps() const
		{
			return (m_targetDuration.count() > 0) ? static_cast<float>(1.0 / std::chrono::duration<double>(m_targetDuration).count()) : 0.0f;
		}

		// Call at the TOP of the game loop, before measuring deltaTime.
		//
		// First invocation per rate setting is always a no-op: it records the
		// first deadline and returns immediately so the first frame renders right away.
		// Every subsequent call sleeps (coarse) then busy-spins (fine) to the deadline.
		void Wait()
		{
			if (m_targetDuration.count() <= 0)
				return;

			if (!m_initialised)
			{
				m_nextFrameTime = Clock::now() + m_targetDuration;
				m_initialised = true;
				return;
			}

			const TimePoint deadline = m_nextFrameTime;
			const Duration remaining = deadline - Clock::now();

			// Coarse sleep: yield the thread until kSpinThreshold before the deadline.
			// This prevents the thread burning a full CPU core for the entire wait.
			if (remaining > kSpinThreshold)
			{
				AE_PROFILE_ZONE_N("FramePacer::Sleep");
				std::this_thread::sleep_for(remaining - kSpinThreshold);
			}

			// Fine busy-spin for sub-millisecond accuracy that sleep_for cannot provide.
			{
				AE_PROFILE_ZONE_N("FramePacer::Spin");
				while (Clock::now() < deadline)
				{ /* spin */
				}
			}

			// Advance the deadline to the next slot.
			// If we ran over budget, clamp to avoid accumulating debt and firing a burst
			// of uncapped frames trying to "catch up".
			m_nextFrameTime = deadline + m_targetDuration;
			if (m_nextFrameTime < Clock::now())
				m_nextFrameTime = Clock::now() + m_targetDuration;
		}

	private:
		// Busy-spin window.  Coarse sleep is used for anything longer than this,
		// then we spin for the final stretch.  2 ms is a safe margin on Windows
		// where timer resolution without timeBeginPeriod can be ~15 ms by default,
		// and ~1-2 ms when the scheduler wakes on a 1 ms period.
		static constexpr Duration kSpinThreshold{ std::chrono::duration_cast<Duration>(std::chrono::milliseconds(2)) };

		Duration m_targetDuration{ 0 };
		TimePoint m_nextFrameTime{};
		bool m_initialised = false;
	};

} // namespace aether
