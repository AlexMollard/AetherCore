#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aether
{
	// One frame's cost, split by phase. All times are milliseconds.
	//
	// wallMs is the honest loop-to-loop measurement and is NEVER clamped; simDtMs is what
	// the simulation actually received after AetherCore's 1/30 ceiling. Showing both is
	// what makes lost simulation time visible instead of hidden behind the clamp.
	struct FrameTiming
	{
		std::uint64_t frameIndex = 0;
		float wallMs = 0.0f;
		float simDtMs = 0.0f;
		float pacerWaitMs = 0.0f;
		float inFlightWaitMs = 0.0f;
		float gameWorkMs = 0.0f;
		float renderExecMs = 0.0f;
		float presentWaitMs = 0.0f;
		// How long the OS event queue sat un-drained between the poll and the input snapshot
		// Tick() takes from it. This is latency the player feels but no frame-time percentile
		// can show, because the frame is vsync-locked either way.
		float inputStaleMs = 0.0f;
		bool renderComplete = false;
	};

	// A fixed ring of per-frame timings, written by the game thread and completed later by
	// the render thread, readable by anyone.
	//
	// Deliberately NOT gated on AE_DEV_TOOLING or TRACY_ENABLE: the engine's existing
	// per-phase timings go to Tracy, which compiles out in Release, so the build that
	// actually ships has no frame data at all. This one always exists. The cost is a few
	// timestamp reads and stores per frame, no allocation and no locks.
	class FrameTimeline
	{
	public:
		static constexpr std::size_t kCapacity = 600; // ~10 s at 60 fps

		// Game thread. Publishes the frame; the record becomes visible to readers.
		void RecordGameFrame(const FrameTiming& timing);

		// Render thread. Fills the two fields the game thread could not know yet, on a
		// record it already published. A frame that has since been evicted is ignored.
		void RecordRenderFrame(std::uint64_t frameIndex, float renderExecMs, float presentWaitMs);

		// Copies the newest frames into `out`, oldest first. Returns how many were written.
		[[nodiscard]] std::size_t Snapshot(std::span<FrameTiming> out) const;

		// Total frames ever recorded, not the number retained.
		[[nodiscard]] std::uint64_t FrameCount() const;

	private:
		// Records are stored at their SEQUENCE position, not at frameIndex % kCapacity.
		// Keying by frame index would make the ring's contents depend on the first index
		// the engine happens to start at, and would leave holes for any gap in the
		// sequence. Sequence order is also what Snapshot needs in order to return frames
		// oldest-first without sorting.
		//
		// The render thread therefore cannot compute a slot directly, and searches back a
		// few records instead. That is bounded and cheap: it trails the game thread by at
		// most Swapchain::kMaxFramesInFlight (currently 3).
		static constexpr std::uint64_t kRenderLookback = 16;

		// The two-writer scheme is only sound because the ring is vastly larger than the
		// render thread's lag. The game thread cannot wrap around and reopen a slot the
		// render thread is still writing. Shrinking kCapacity toward the in-flight count
		// would reintroduce a data race, so the relationship is asserted rather than left
		// to a comment.
		static_assert(kCapacity >= 64, "FrameTimeline capacity must stay far above the render thread's frame lag");
		static_assert(kCapacity > kRenderLookback, "The render lookback must stay inside the ring");

		struct Slot
		{
			std::uint64_t frameIndex = 0;
			float wallMs = 0.0f;
			float simDtMs = 0.0f;
			float pacerWaitMs = 0.0f;
			float inFlightWaitMs = 0.0f;
			float gameWorkMs = 0.0f;
			// Written by the render thread after the record is published, so these are the
			// only fields two threads touch.
			std::atomic<float> renderExecMs{0.0f};
			std::atomic<float> presentWaitMs{0.0f};
			std::atomic<bool> renderComplete{false};
		};

		std::array<Slot, kCapacity> m_slots{};
		std::atomic<std::uint64_t> m_recorded{0};
	};
} // namespace aether
