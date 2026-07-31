#include "utils/FrameTimeline.hpp"

#include <algorithm>

namespace aether
{
	void FrameTimeline::RecordGameFrame(const FrameTiming& timing)
	{
		// Sequence position, not frameIndex: see the note on kRenderLookback. This keeps the
		// ring dense and ordered regardless of what index the engine started counting from.
		const std::uint64_t seq = m_recorded.load(std::memory_order_relaxed);
		Slot& slot = m_slots[seq % kCapacity];

		// Retire the previous occupant's render half BEFORE publishing, so a reader can
		// never see this frame's game data paired with the last one's render data.
		slot.renderComplete.store(false, std::memory_order_relaxed);
		slot.renderExecMs.store(0.0f, std::memory_order_relaxed);
		slot.presentWaitMs.store(0.0f, std::memory_order_relaxed);

		slot.frameIndex = timing.frameIndex;
		slot.wallMs = timing.wallMs;
		slot.simDtMs = timing.simDtMs;
		slot.pacerWaitMs = timing.pacerWaitMs;
		slot.inFlightWaitMs = timing.inFlightWaitMs;
		slot.gameWorkMs = timing.gameWorkMs;

		// Release: everything above is visible to any reader that acquires m_recorded.
		m_recorded.store(seq + 1, std::memory_order_release);
	}

	void FrameTimeline::RecordRenderFrame(const std::uint64_t frameIndex, const float renderExecMs, const float presentWaitMs)
	{
		const std::uint64_t recorded = m_recorded.load(std::memory_order_acquire);
		const std::uint64_t lookback = std::min<std::uint64_t>(recorded, kRenderLookback);
		for (std::uint64_t i = 0; i < lookback; ++i)
		{
			const std::uint64_t seq = recorded - 1 - i;
			Slot& slot = m_slots[seq % kCapacity];
			if (slot.frameIndex != frameIndex)
			{
				continue;
			}
			slot.renderExecMs.store(renderExecMs, std::memory_order_relaxed);
			slot.presentWaitMs.store(presentWaitMs, std::memory_order_relaxed);
			slot.renderComplete.store(true, std::memory_order_release);
			return;
		}
		// Older than the lookback: the frame has been evicted, or this is a completion for
		// a frame the timeline never saw. Dropping it is correct - writing it somewhere
		// would corrupt an unrelated record.
	}

	std::size_t FrameTimeline::Snapshot(const std::span<FrameTiming> out) const
	{
		const std::uint64_t recorded = m_recorded.load(std::memory_order_acquire);
		if (recorded == 0 || out.empty())
		{
			return 0;
		}

		const std::uint64_t available = std::min<std::uint64_t>(recorded, kCapacity);
		const std::uint64_t wanted = std::min<std::uint64_t>(available, out.size());
		const std::uint64_t firstSeq = recorded - wanted;

		for (std::uint64_t i = 0; i < wanted; ++i)
		{
			const Slot& slot = m_slots[(firstSeq + i) % kCapacity];
			FrameTiming& dst = out[static_cast<std::size_t>(i)];
			dst.frameIndex = slot.frameIndex;
			dst.wallMs = slot.wallMs;
			dst.simDtMs = slot.simDtMs;
			dst.pacerWaitMs = slot.pacerWaitMs;
			dst.inFlightWaitMs = slot.inFlightWaitMs;
			dst.gameWorkMs = slot.gameWorkMs;
			dst.renderExecMs = slot.renderExecMs.load(std::memory_order_relaxed);
			dst.presentWaitMs = slot.presentWaitMs.load(std::memory_order_relaxed);
			dst.renderComplete = slot.renderComplete.load(std::memory_order_acquire);
		}
		return static_cast<std::size_t>(wanted);
	}

	std::uint64_t FrameTimeline::FrameCount() const
	{
		return m_recorded.load(std::memory_order_acquire);
	}
} // namespace aether
