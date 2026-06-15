#include "rendering/GpuTimestampPool.hpp"

#include <algorithm>

#include "gpu/GpuDeviceFactory.hpp"
#include "utils/Logger.hpp"
#include "utils/LogCategory.hpp"

namespace aether
{
	void GpuTimestampPool::Initialize(gpu::Device device, gpu::PhysicalDevice physDevice)
	{
		m_device = device;

		const auto props = gpu::Factory::GetPhysicalDeviceProperties(physDevice);

		if (!props.limits.timestampComputeAndGraphics)
		{
			AE_WARN(LogCategory::Vulkan, "GpuTimestampPool: device does not support timestamps on all queues - pool disabled.");
			m_device = nullptr;
			return;
		}

		m_periodNs = props.limits.timestampPeriod;

		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			m_pools[i] = gpu::Factory::CreateQueryPool(device,
			        {
			                .type = gpu::Factory::QueryType::Timestamp,
			                .count = kMaxTimestamps,
			        });
			if (m_pools[i] == nullptr)
			{
				AE_WARN(LogCategory::Vulkan, "GpuTimestampPool: failed to create query pool [{}] - pool disabled.", i);
				Shutdown();
				return;
			}
			// Initial host-side reset so the pool is in a defined state before first use.
			gpu::Factory::ResetQueryPool(device, m_pools[i], 0, kMaxTimestamps);
		}
	}

	void GpuTimestampPool::Shutdown()
	{
		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			if (m_pools[i] != nullptr)
			{
				gpu::Factory::DestroyQueryPool(m_device, m_pools[i]);
				m_pools[i] = nullptr;
			}
			m_writeCount[i] = 0;
			m_hasData[i] = false;
		}
		m_device = nullptr;
	}

	std::uint32_t GpuTimestampPool::BeginFrame(std::uint32_t frameIndex, std::span<float> outMs)
	{
		if (m_device == nullptr)
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
				const std::uint32_t read = gpu::Factory::GetQueryPoolResults(m_device, m_pools[m_currentSlot], 0, count, std::span<std::uint64_t>(rawTicks, count));

				if (read > 0)
				{
					for (std::uint32_t i = 0; i < read; ++i)
					{
						outMs[i] = static_cast<float>(rawTicks[i]) * m_periodNs * 1e-6f;
					}
					validCount = read;
				}
			}
		}

		// Reset this slot for new writes.
		gpu::Factory::ResetQueryPool(m_device, m_pools[m_currentSlot], 0, kMaxTimestamps);
		m_writeCount[m_currentSlot] = 0;
		m_hasData[m_currentSlot] = false;

		return validCount;
	}

	std::uint32_t GpuTimestampPool::Write(gpu::CommandList& cmdList, gpu::PipelineStage stage)
	{
		if (m_device == nullptr)
		{
			return 0;
		}

		const std::uint32_t slot = m_writeCount[m_currentSlot];
		if (slot >= kMaxTimestamps)
		{
			return slot;
		}

		cmdList.WriteTimestamp(static_cast<void*>(m_pools[m_currentSlot]), slot, stage);
		m_writeCount[m_currentSlot] = slot + 1;
		m_hasData[m_currentSlot] = true;
		return slot;
	}
} // namespace aether
