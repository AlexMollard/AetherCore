#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "streaming/StreamingTypes.hpp"
#include "utils/Expected.hpp"

namespace aether::streaming
{
	enum class MemoryClass
	{
		CpuResident,
		CpuPending,
		CpuRetiring,
		GpuResident,
		GpuPending,
		GpuRetiring,
		Staging
	};

	class BudgetLedger
	{
	public:
		Expected<void> Configure(const Budgets& budgets);
		[[nodiscard]] bool TryReserve(MemoryClass memoryClass, std::uint64_t bytes);
		void Release(MemoryClass memoryClass, std::uint64_t bytes);
		[[nodiscard]] bool Transfer(MemoryClass from, MemoryClass to, std::uint64_t bytes);
		[[nodiscard]] std::uint64_t Bytes(MemoryClass memoryClass) const;
		[[nodiscard]] Budgets Limits() const;

	private:
		bool Fits(const std::array<std::uint64_t, 7>& amounts, MemoryClass target) const;
		mutable std::mutex m_mutex;
		Budgets m_limits;
		std::array<std::uint64_t, 7> m_bytes{};
	};
}
