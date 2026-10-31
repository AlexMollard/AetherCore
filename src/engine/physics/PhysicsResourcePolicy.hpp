#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace aether
{
	// Physics-job dependencies order allocation/free operations, including across
	// worker threads. Read this accounting only after joining the fixed step.
	class PhysicsScratchBudget
	{
	public:
		explicit PhysicsScratchBudget(std::size_t capacity): m_capacity(capacity) {}
		bool TryAcquire(std::size_t bytes)
		{
			if (bytes > m_capacity - m_live) { return false; }
			m_live += bytes;
			m_high_water = std::max(m_high_water, m_live);
			return true;
		}
		void Release(std::size_t bytes) { m_live -= bytes; }
		[[nodiscard]] std::size_t LiveBytes() const { return m_live; }
		[[nodiscard]] std::size_t HighWaterBytes() const { return m_high_water; }
	private:
		std::size_t m_capacity;
		std::size_t m_live = 0;
		std::size_t m_high_water = 0;
	};

	struct PhysicsSaturationStats
	{
		std::uint64_t steps = 0;
		std::uint64_t saturatedSteps = 0;
		std::uint64_t manifoldCacheFullSteps = 0;
		std::uint64_t bodyPairCacheFullSteps = 0;
		std::uint64_t contactConstraintsFullSteps = 0;
		std::uint64_t diagnostics = 0;
		std::uint64_t lastDiagnosticStep = 0;
		std::uint32_t lastErrorFlags = 0;
		std::uint32_t observedErrorFlags = 0;
	};

	// Bit positions match pinned Jolt 5.5.0 EPhysicsUpdateError (checked at use).
	inline bool RecordPhysicsSaturation(PhysicsSaturationStats& stats, std::uint32_t flags)
	{
		++stats.steps;
		stats.lastErrorFlags = flags;
		if (!flags) { return false; }
		++stats.saturatedSteps;
		if (flags & 1u) { ++stats.manifoldCacheFullSteps; }
		if (flags & 2u) { ++stats.bodyPairCacheFullSteps; }
		if (flags & 4u) { ++stats.contactConstraintsFullSteps; }
		const bool diagnose = (flags & ~stats.observedErrorFlags) != 0 || stats.steps - stats.lastDiagnosticStep >= 60;
		stats.observedErrorFlags |= flags;
		if (diagnose)
		{
			++stats.diagnostics;
			stats.lastDiagnosticStep = stats.steps;
		}
		return diagnose;
	}

	struct PhysicsResourceStats
	{
		PhysicsSaturationStats saturation;
		std::size_t scratchHighWaterBytes = 0;
		std::size_t scratchFallbackHighWaterBytes = 0;
		std::size_t scratchFallbackLiveBytes = 0;
		std::size_t scratchArenaCapacityBytes = 0;
		std::size_t scratchFallbackCapacityBytes = 0;
		std::uint64_t scratchFallbackAllocations = 0;
	};
}
