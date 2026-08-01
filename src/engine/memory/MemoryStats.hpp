#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "memory/MemoryTag.hpp"

namespace aether::memory
{
	struct TagTotals
	{
		std::uint64_t currentBytes = 0;
		std::uint64_t peakBytes = 0;
		std::uint64_t totalAllocations = 0;
		std::uint64_t totalFrees = 0;

		// Allocations that have not been freed. Saturating rather than wrapping: the counters
		// are read without a lock, so a reader can legitimately observe a free whose matching
		// allocation it has not seen yet, and an unsigned underflow there would report
		// billions of live blocks instead of approximately zero.
		[[nodiscard]] std::uint64_t LiveCount() const noexcept
		{
			return totalAllocations > totalFrees ? totalAllocations - totalFrees : 0;
		}
	};

	// Per-tag counters, updated on every tracked allocation in every configuration.
	//
	// This sits on the hot path, so each tag's counters are isolated to their own cache line:
	// two threads allocating under different tags otherwise ping-pong the same line between
	// cores and the counters cost far more than the allocation they are measuring.
	//
	// Relaxed ordering throughout. These are statistics, not synchronisation - no reader
	// depends on seeing them ordered against anything else, and the stronger orderings would
	// put a barrier in the allocation path to buy nothing.
	class MemoryStats
	{
	public:
		void RecordAllocation(MemTag tag, std::size_t bytes) noexcept;
		void RecordFree(MemTag tag, std::size_t bytes) noexcept;

		[[nodiscard]] TagTotals Get(MemTag tag) const noexcept;

		// Summed across tags. Not a consistent snapshot: tags are read one at a time while
		// other threads keep allocating, so the total is an approximation by construction.
		[[nodiscard]] TagTotals Total() const noexcept;

		// Peaks only; live bytes and lifetime counts are left alone. Used to measure a
		// specific window, such as a level load, without disturbing what is currently held.
		void ResetPeaks() noexcept;

	private:
		// 64 bytes on every architecture this ships to. Deliberately not
		// hardware_destructive_interference_size: that varies by standard-library version and
		// would silently change this type's layout and size between toolchains.
		static constexpr std::size_t kCacheLine = 64;

		struct alignas(kCacheLine) TagCounters
		{
			std::atomic<std::uint64_t> currentBytes{0};
			std::atomic<std::uint64_t> peakBytes{0};
			std::atomic<std::uint64_t> totalAllocations{0};
			std::atomic<std::uint64_t> totalFrees{0};
		};

		static_assert(sizeof(TagCounters) == kCacheLine, "TagCounters must occupy exactly one cache line");

		TagCounters m_tags[kMemTagCount];
	};

	// The process-wide instance.
	//
	// A function-local static would be initialized on first use, and first use here is the
	// first allocation in the process - which can happen during static initialization, before
	// any guard could run. This has to be constant-initialized instead, so it is simply valid
	// from the moment the process starts.
	[[nodiscard]] MemoryStats& GlobalStats() noexcept;
} // namespace aether::memory
