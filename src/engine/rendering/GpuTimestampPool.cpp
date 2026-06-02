#include "rendering/GpuTimestampPool.hpp"

#include "utils/Logger.hpp"
#include "utils/LogCategory.hpp"

namespace aether
{
	void GpuTimestampPool::Initialize(VkDevice device, VkPhysicalDevice physDevice)
	{
		m_device = device;

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physDevice, &props);

		if (props.limits.timestampComputeAndGraphics == VK_FALSE)
		{
			AE_WARN(LogCategory::Vulkan, "GpuTimestampPool: device does not support timestamps on all queues - pool disabled.");
			m_device = VK_NULL_HANDLE;
			return;
		}

		m_periodNs = props.limits.timestampPeriod;

		const VkQueryPoolCreateInfo createInfo{
		        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		        .queryType = VK_QUERY_TYPE_TIMESTAMP,
		        .queryCount = kMaxTimestamps,
		};

		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			if (vkCreateQueryPool(device, &createInfo, nullptr, &m_pools[i]) != VK_SUCCESS)
			{
				AE_WARN(LogCategory::Vulkan, "GpuTimestampPool: failed to create query pool [{}] - pool disabled.", i);
				Shutdown();
				return;
			}
			// Initial host-side reset so the pool is in a defined state before first use.
			vkResetQueryPool(device, m_pools[i], 0, kMaxTimestamps);
		}
	}

	void GpuTimestampPool::Shutdown()
	{
		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			if (m_pools[i] != VK_NULL_HANDLE)
			{
				vkDestroyQueryPool(m_device, m_pools[i], nullptr);
				m_pools[i] = VK_NULL_HANDLE;
			}
			m_writeCount[i] = 0;
			m_hasData[i] = false;
		}
		m_device = VK_NULL_HANDLE;
	}

	std::uint32_t GpuTimestampPool::BeginFrame(std::uint32_t frameIndex, std::span<float> outMs)
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return 0;
		}

		m_currentSlot = frameIndex % kFramesInFlight;

		// Read back results from the previous use of this slot (kFramesInFlight frames ago).
		std::uint32_t validCount = 0;
		if (m_hasData[m_currentSlot] && !outMs.empty())
		{
			const std::uint32_t count = std::min(m_writeCount[m_currentSlot], static_cast<std::uint32_t>(outMs.size()));
			if (count > 0)
			{
				std::uint64_t rawTicks[kMaxTimestamps]{};
				const VkResult result = vkGetQueryPoolResults(m_device, m_pools[m_currentSlot], 0, count, count * sizeof(std::uint64_t), rawTicks, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);

				if (result == VK_SUCCESS)
				{
					for (std::uint32_t i = 0; i < count; ++i)
					{
						outMs[i] = static_cast<float>(rawTicks[i]) * m_periodNs * 1e-6f;
					}
					validCount = count;
				}
			}
		}

		// Reset this slot for new writes.
		vkResetQueryPool(m_device, m_pools[m_currentSlot], 0, kMaxTimestamps);
		m_writeCount[m_currentSlot] = 0;
		m_hasData[m_currentSlot] = false;

		return validCount;
	}

	std::uint32_t GpuTimestampPool::Write(VkCommandBuffer cmd, VkPipelineStageFlagBits2 stage)
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return 0;
		}

		const std::uint32_t slot = m_writeCount[m_currentSlot];
		if (slot >= kMaxTimestamps)
		{
			return slot;
		}

		vkCmdWriteTimestamp2(cmd, stage, m_pools[m_currentSlot], slot);
		m_writeCount[m_currentSlot] = slot + 1;
		m_hasData[m_currentSlot] = true;
		return slot;
	}
} // namespace aether
