#pragma once

#include <cstdint>
#include <vector>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	// A slot allocator that defers reuse of a freed slot until several frames
	// have elapsed, so the slot's backing GPU memory is never handed out again
	// while an in-flight frame may still reference it. Mirrors the deferred-free
	// idiom used by BindlessManager / ImguiSubsystem for the same reason.
	//
	// Thread safety: NOT thread-safe. The owner (MaterialBuffer) serializes all
	// calls under its own mutex.
	class DeferredSlotFreeList
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		// A slot freed on frame F becomes reusable once the frame index advances
		// to F + kReuseDelayFrames. kMaxFramesInFlight covers the frames whose
		// command buffers may still reference the slot; +1 is a guard frame.
		static constexpr std::uint64_t kReuseDelayFrames = kMaxFramesInFlight + 1u;

		// Populate the free list with slots [0, capacity) and clear all state.
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

		// Release all bookkeeping (used on shutdown).
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

		// Queue a slot for reuse kReuseDelayFrames from the current frame. The
		// slot is NOT immediately available to Allocate().
		void Free(std::uint32_t slot)
		{
			m_retire.push_back(Retired{slot, m_frameIndex + kReuseDelayFrames});
		}

		// Advance to frameIndex and return any slots whose delay has elapsed to
		// the free list.
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
