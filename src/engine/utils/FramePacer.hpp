#pragma once

#include <chrono>
#include <thread>

#include "utils/Profiler.hpp"

namespace aether
{
	// Regulates game-thread cadence to a fixed target frame rate.
	class FramePacer
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;
		using Duration = Clock::duration;

		void SetTargetFps(float fps)
		{
			if (fps > 0.0f)
			{
				m_targetDuration = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
			}
			else
			{
				m_targetDuration = Duration{0};
			}
			m_initialised = false;
		}

		[[nodiscard]] float GetTargetFps() const
		{
			return (m_targetDuration.count() > 0) ? static_cast<float>(1.0 / std::chrono::duration<double>(m_targetDuration).count()) : 0.0f;
		}

		void Wait()
		{
			if (m_targetDuration.count() <= 0)
			{
				return;
			}

			if (!m_initialised)
			{
				m_nextFrameTime = Clock::now() + m_targetDuration;
				m_initialised = true;
				return;
			}

			const TimePoint deadline = m_nextFrameTime;
			const Duration remaining = deadline - Clock::now();

			// Coarse sleep: yield the thread until kSpinThreshold before the deadline.
			if (remaining > kSpinThreshold)
			{
				AE_PROFILE_ZONE_N("FramePacer::Sleep");
				std::this_thread::sleep_for(remaining - kSpinThreshold);
			}

			{
				AE_PROFILE_ZONE_N("FramePacer::Spin");
				while (Clock::now() < deadline)
				{ /* spin */
				}
			}

			m_nextFrameTime = deadline + m_targetDuration;
			if (m_nextFrameTime < Clock::now())
			{
				m_nextFrameTime = Clock::now() + m_targetDuration;
			}
		}

	private:
		static constexpr Duration kSpinThreshold{std::chrono::duration_cast<Duration>(std::chrono::milliseconds(2))};

		Duration m_targetDuration{0};
		TimePoint m_nextFrameTime;
		bool m_initialised = false;
	};

} // namespace aether
