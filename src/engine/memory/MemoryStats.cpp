#include "memory/MemoryStats.hpp"

#include <algorithm>

namespace aether::memory
{
	namespace
	{
		// Constant-initialized: this must be usable from the first allocation in the process,
		// which can occur during static initialization of another translation unit. A
		// function-local static would introduce a guard variable and an initialization order
		// this cannot afford on the allocation path.
		constinit MemoryStats g_stats;

		[[nodiscard]] std::size_t IndexOf(const MemTag tag) noexcept
		{
			const auto index = static_cast<std::size_t>(tag);
			return index < kMemTagCount ? index : static_cast<std::size_t>(MemTag::Unknown);
		}
	} // namespace

	void MemoryStats::RecordAllocation(const MemTag tag, const std::size_t bytes) noexcept
	{
		TagCounters& counters = m_tags[IndexOf(tag)];
		const std::uint64_t updated = counters.currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
		counters.totalAllocations.fetch_add(1, std::memory_order_relaxed);

		// Compare-exchange rather than a plain store: two threads can both observe a peak
		// lower than their own total, and the loser of a naive store would drag the recorded
		// peak back down below a value that genuinely occurred.
		std::uint64_t peak = counters.peakBytes.load(std::memory_order_relaxed);
		while (peak < updated && !counters.peakBytes.compare_exchange_weak(peak, updated, std::memory_order_relaxed, std::memory_order_relaxed))
		{
		}
	}

	void MemoryStats::RecordFree(const MemTag tag, const std::size_t bytes) noexcept
	{
		TagCounters& counters = m_tags[IndexOf(tag)];
		counters.currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
		counters.totalFrees.fetch_add(1, std::memory_order_relaxed);
	}

	TagTotals MemoryStats::Get(const MemTag tag) const noexcept
	{
		const TagCounters& counters = m_tags[IndexOf(tag)];
		return {
		        .currentBytes = counters.currentBytes.load(std::memory_order_relaxed),
		        .peakBytes = counters.peakBytes.load(std::memory_order_relaxed),
		        .totalAllocations = counters.totalAllocations.load(std::memory_order_relaxed),
		        .totalFrees = counters.totalFrees.load(std::memory_order_relaxed),
		};
	}

	TagTotals MemoryStats::Total() const noexcept
	{
		TagTotals total;
		for (std::size_t i = 0; i < kMemTagCount; ++i)
		{
			const TagTotals tag = Get(static_cast<MemTag>(i));
			total.currentBytes += tag.currentBytes;
			// Summed, not maxed: the question this answers is "how much did each part of the
			// engine peak at", and those peaks did not necessarily coincide. It is therefore
			// an upper bound on the process peak, never a measurement of it.
			total.peakBytes += tag.peakBytes;
			total.totalAllocations += tag.totalAllocations;
			total.totalFrees += tag.totalFrees;
		}
		return total;
	}

	void MemoryStats::ResetPeaks() noexcept
	{
		for (std::size_t i = 0; i < kMemTagCount; ++i)
		{
			TagCounters& counters = m_tags[i];
			// Reset to what is currently held rather than to zero: those bytes are live right
			// now, so a peak below them would be a figure that never occurred.
			counters.peakBytes.store(counters.currentBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
	}

	MemoryStats& GlobalStats() noexcept
	{
		return g_stats;
	}
} // namespace aether::memory
