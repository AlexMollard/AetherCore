#include "vulkan/GpuHeap.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "utils/Expected.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void GpuHeap::Initialize(const VulkanContext& ctx, Desc desc)
	{
		m_allocatorRef = ctx.GetAllocator();
		m_deviceRef = ctx.GetDevice().device;

		constexpr VkBufferUsageFlags kBaseUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		AE_EXPECT_OR_THROW(buffer, UniqueBuffer::CreateDeviceLocal(m_allocatorRef, m_deviceRef, desc.capacityBytes, kBaseUsage | desc.additionalUsage));
		m_buffer = std::move(buffer);
		m_freeList.push_back({ 0, desc.capacityBytes });
	}

	void GpuHeap::Shutdown()
	{
		m_buffer.Reset();
		m_freeList.clear();
	}

	VkDeviceSize GpuHeap::AllocBytes(VkDeviceSize bytes)
	{
		for (auto it = m_freeList.begin(); it != m_freeList.end(); ++it)
		{
			if (it->size >= bytes)
			{
				const VkDeviceSize offset = it->offset;
				if (it->size == bytes)
				{
					m_freeList.erase(it);
				}
				else
				{
					it->offset += bytes;
					it->size -= bytes;
				}
				return offset;
			}
		}
		return kInvalidOffset;
	}

	void GpuHeap::FreeBytes(VkDeviceSize offset, VkDeviceSize bytes)
	{
		auto it = std::lower_bound(m_freeList.begin(), m_freeList.end(), offset, [](const FreeBlock& b, VkDeviceSize o) { return b.offset < o; });
		it = m_freeList.insert(it, { offset, bytes });

		// Merge with next block if adjacent.
		if (const auto next = std::next(it); next != m_freeList.end() && it->offset + it->size == next->offset)
		{
			it->size += next->size;
			m_freeList.erase(next);
		}

		// Merge with previous block if adjacent.
		if (it != m_freeList.begin())
		{
			const auto prev = std::prev(it);
			if (prev->offset + prev->size == it->offset)
			{
				prev->size += it->size;
				m_freeList.erase(it);
			}
		}
	}

	void GpuHeap::UploadBytes(VkDeviceAddress dstAddr, const void* src, VkDeviceSize bytes, VkDevice device, VkQueue queue, VkCommandPool pool)
	{
		assert(dstAddr >= m_buffer.GetDeviceAddress());
		const VkDeviceSize dstOffset = dstAddr - m_buffer.GetDeviceAddress();

		// Transient host-visible staging buffer.
		AE_EXPECT_OR_THROW(staging, UniqueBuffer::CreateMapped(m_allocatorRef, device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
		std::memcpy(staging.GetAllocationInfo().pMappedData, src, static_cast<std::size_t>(bytes));
		AE_EXPECT_OR_THROW_VOID(staging.FlushMapped());

		// One-time command buffer.
		const VkCommandBufferAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		const VkResult allocResult = vkAllocateCommandBuffers(device, &allocInfo, &cmd);
		if (allocResult != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(static_cast<int32_t>(allocResult), "GpuHeap: failed to allocate upload command buffer"));
		}

		const VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		const VkResult beginResult = vkBeginCommandBuffer(cmd, &beginInfo);
		if (beginResult != VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, pool, 1, &cmd);
			Throw(AetherError::Vulkan(static_cast<int32_t>(beginResult), "GpuHeap: failed to begin upload command buffer"));
		}

		const VkBufferCopy region{ .srcOffset = 0, .dstOffset = dstOffset, .size = bytes };
		vkCmdCopyBuffer(cmd, staging.Get(), m_buffer.Get(), 1, &region);

		const VkResult endResult = vkEndCommandBuffer(cmd);
		if (endResult != VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, pool, 1, &cmd);
			Throw(AetherError::Vulkan(static_cast<int32_t>(endResult), "GpuHeap: failed to end upload command buffer"));
		}

		const VkSubmitInfo submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
		};
		const VkResult submitResult = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
		if (submitResult != VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, pool, 1, &cmd);
			Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "GpuHeap: failed to submit upload command buffer"));
		}
		const VkResult idleResult = vkQueueWaitIdle(queue);
		if (idleResult != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(static_cast<int32_t>(idleResult), "GpuHeap: failed to wait idle after upload"));
		}
		vkFreeCommandBuffers(device, pool, 1, &cmd);
	}
} // namespace aether
