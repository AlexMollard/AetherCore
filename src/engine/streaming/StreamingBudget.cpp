#include "streaming/StreamingBudget.hpp"

#include <cassert>
#include <limits>

namespace aether::streaming
{
	namespace
	{
		constexpr std::size_t Index(MemoryClass value) { return static_cast<std::size_t>(value); }
		bool Cpu(MemoryClass value) { return value == MemoryClass::Staging || Index(value) <= Index(MemoryClass::CpuRetiring); }
		bool Within(std::uint64_t limit, const std::array<std::uint64_t, 7>& amounts, bool cpu)
		{
			for (std::size_t i = 0; i < amounts.size(); ++i)
			{
				if (Cpu(static_cast<MemoryClass>(i)) != cpu) continue;
				if (amounts[i] > limit) return false;
				limit -= amounts[i];
			}
			return true;
		}
	}

	Expected<void> BudgetLedger::Configure(const Budgets& budgets)
	{
		std::lock_guard lock(m_mutex);
		m_limits = budgets;
		return {};
	}

	bool BudgetLedger::Fits(const std::array<std::uint64_t, 7>& amounts, MemoryClass target) const
	{
		return Within(Cpu(target) ? m_limits.cpuBytes : m_limits.gpuBytes, amounts, Cpu(target)) &&
			(target != MemoryClass::Staging || amounts[Index(target)] <= m_limits.stagingBytes);
	}

	bool BudgetLedger::TryReserve(MemoryClass memoryClass, std::uint64_t bytes)
	{
		std::lock_guard lock(m_mutex);
		const auto index = Index(memoryClass);
		if (index >= m_bytes.size() || bytes > std::numeric_limits<std::uint64_t>::max() - m_bytes[index]) return false;
		auto next = m_bytes;
		next[index] += bytes;
		if (!Fits(next, memoryClass)) return false;
		m_bytes = next;
		return true;
	}

	void BudgetLedger::Release(MemoryClass memoryClass, std::uint64_t bytes)
	{
		std::lock_guard lock(m_mutex);
		const auto index = Index(memoryClass);
		assert(index < m_bytes.size() && bytes <= m_bytes[index]);
		if (index >= m_bytes.size() || bytes > m_bytes[index]) return;
		m_bytes[index] -= bytes;
	}

	bool BudgetLedger::Transfer(MemoryClass from, MemoryClass to, std::uint64_t bytes)
	{
		std::lock_guard lock(m_mutex);
		const auto source = Index(from), target = Index(to);
		if (source >= m_bytes.size() || target >= m_bytes.size() || bytes > m_bytes[source]) return false;
		if (source == target) return true;
		if (bytes > std::numeric_limits<std::uint64_t>::max() - m_bytes[target]) return false;
		auto next = m_bytes;
		next[source] -= bytes;
		next[target] += bytes;
		if (Cpu(from) != Cpu(to) && !Fits(next, to)) return false;
		if (to == MemoryClass::Staging && next[target] > m_limits.stagingBytes) return false;
		m_bytes = next;
		return true;
	}

	std::uint64_t BudgetLedger::Bytes(MemoryClass memoryClass) const
	{
		std::lock_guard lock(m_mutex);
		const auto index = Index(memoryClass);
		return index < m_bytes.size() ? m_bytes[index] : 0;
	}

	Budgets BudgetLedger::Limits() const
	{
		std::lock_guard lock(m_mutex);
		return m_limits;
	}
}
