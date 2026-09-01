#pragma once

#include <algorithm>

namespace aether
{
	// When to latch input, relative to the flip that will show the frame.
	//
	// This is Unreal's r.GTSyncType 2: the game thread is kicked off a fixed number of
	// milliseconds before the predicted vsync (their rhi.SyncSlackMS, default 10 ms). The
	// slack is a CONSTANT, not something derived from measured work.
	//
	// It used to be adaptive here - work x 2, ratcheted up on a missed flip and decayed at
	// 1% a frame - and that is precisely what broke it. A miss raised the reserve, a raised
	// reserve put the latch point in the past so the frame was not paced, and an unpaced
	// frame missed again. Measured latched up at 16.30 ms against a 16.44 ms period with
	// paced=1 in 2400 frames. A constant cannot do that.
	//
	// Lower slack = less input latency and less room for a frame that runs long. That trade
	// belongs to whoever is using the engine, so it is a setting rather than a constant.
	class LatencyPacer
	{
	public:
		// Unreal's default. Deliberately conservative: it is the value a shipping title gets
		// before anyone tunes it.
		static constexpr float kDefaultSlackMs = 10.0f;

		void SetSlackMs(const float slackMs) noexcept
		{
			m_slackMs = std::max(0.0f, slackMs);
		}

		// How long before the predicted flip to latch input, given the display period.
		//
		// Held below the interval because a slack of a whole interval puts the latch point on
		// the PREVIOUS flip, which is always in the past - pacing then never engages at all.
		[[nodiscard]] float ReserveMs(const float intervalMs) const noexcept
		{
			if (!(intervalMs > 0.0f))
			{
				return 0.0f;
			}
			return std::min(m_slackMs, intervalMs * kMaxSlackFraction);
		}

	private:
		// Leaves a tenth of the interval so the latch point is always in the future.
		static constexpr float kMaxSlackFraction = 0.9f;

		float m_slackMs = kDefaultSlackMs;
	};
} // namespace aether
