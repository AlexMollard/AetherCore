#pragma once

#include <cstdint>
#include <span>
#include "vulkan/volk.hpp"

namespace aether
{
	// Lightweight per-frame GPU timestamp pool.
	//
	// One VkQueryPool per frame-in-flight slot. Each slot is reset, written, and
	// read back in a ring: by the time BeginFrame(N) runs, the CPU has already
	// waited on the fence for slot N % kFramesInFlight (3 frames ago), so reading
	// and resetting that slot is safe without any extra synchronization.
	//
	// Usage:
	//   pool.Initialize(device, physDevice);
	//   // each frame:
	//   float results[GpuTimestampPool::kMaxTimestamps];
	//   uint32_t count = pool.BeginFrame(frameIndex, results); // read previous, reset slot
	//   pool.Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);  // returns slot index
	//   pool.Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
	//   // ...submit command buffer...
	class GpuTimestampPool
	{
	public:
		static constexpr std::uint32_t kMaxTimestamps = 16;
		static constexpr std::uint32_t kFramesInFlight = 3;

		void Initialize(VkDevice device, VkPhysicalDevice physDevice);
		void Shutdown();

		// Call at the start of each frame before recording commands.
		// Reads back completed results for this slot (written kFramesInFlight frames ago)
		// into outMs (must have capacity >= kMaxTimestamps), resets the slot for new writes.
		// Returns the number of valid millisecond values placed in outMs.
		// Returns 0 on the first kFramesInFlight frames (no completed data yet).
		std::uint32_t BeginFrame(std::uint32_t frameIndex, std::span<float> outMs);

		// Record a timestamp into the current frame slot.
		// Returns the slot index (pass the pair of indices to compute a duration).
		std::uint32_t Write(VkCommandBuffer cmd, VkPipelineStageFlagBits2 stage);

		[[nodiscard]] bool IsValid() const
		{
			return m_device != VK_NULL_HANDLE;
		}

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VkQueryPool m_pools[kFramesInFlight]{};
		float m_periodNs = 1.f;
		std::uint32_t m_writeCount[kFramesInFlight]{};
		bool m_hasData[kFramesInFlight]{};
		std::uint32_t m_currentSlot = 0;
	};
} // namespace aether
