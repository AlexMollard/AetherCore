#pragma once

#include <cstdint>
#include <vector>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	// have elapsed, so the slot's backing GPU memory is never handed out again
	class DeferredSlotFreeList
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		static constexpr std::uint64_t kReuseDelayFrames = kMaxFramesInFlight + 1u;

		void Reset(std::uint32_t capacity)
		{
			m_free.clear();
			m_retire.clear();
			m_frameIndex = 0;
			m_free.reserve(capacity);
			for (std::uint32_t i = capacity; i-- > 0;)
			{
				m_free.push_back(i);
			}
		}

		void Clear()
		{
			m_free.clear();
			m_retire.clear();
			m_frameIndex = 0;
		}

		[[nodiscard]] std::uint32_t Allocate()
		{
			if (m_free.empty())
			{
				return kInvalidSlot;
			}
			const std::uint32_t slot = m_free.back();
			m_free.pop_back();
			return slot;
		}

		void Free(std::uint32_t slot)
		{
			m_retire.push_back(Retired{slot, m_frameIndex + kReuseDelayFrames});
		}

		void AdvanceFrame(std::uint64_t frameIndex)
		{
			m_frameIndex = frameIndex;
			for (std::size_t i = 0; i < m_retire.size();)
			{
				if (m_retire[i].retireFrame <= m_frameIndex)
				{
					m_free.push_back(m_retire[i].slot);
					m_retire[i] = m_retire.back();
					m_retire.pop_back();
				}
				else
				{
					++i;
				}
			}
		}

		[[nodiscard]] std::size_t FreeCount() const
		{
			return m_free.size();
		}

		[[nodiscard]] std::size_t PendingCount() const
		{
			return m_retire.size();
		}

	private:
		struct Retired
		{
			std::uint32_t slot = 0;
			std::uint64_t retireFrame = 0;
		};

		std::vector<std::uint32_t> m_free;
		std::vector<Retired> m_retire;
		std::uint64_t m_frameIndex = 0;
	};
} // namespace aether
