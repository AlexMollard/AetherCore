#include "GpuHeap.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

#include "VulkanContext.hpp"

namespace aether
{
	void GpuHeap::Initialize(const VulkanContext& ctx, Desc desc)
	{
		m_allocatorRef = ctx.GetAllocator();
		m_deviceRef = ctx.GetDevice().device;

		constexpr VkBufferUsageFlags kBaseUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		m_buffer = UniqueBuffer::CreateDeviceLocal(m_allocatorRef, m_deviceRef, desc.capacityBytes, kBaseUsage | desc.additionalUsage);
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
		UniqueBuffer staging = UniqueBuffer::CreateMapped(m_allocatorRef, device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		std::memcpy(staging.GetAllocationInfo().pMappedData, src, static_cast<std::size_t>(bytes));
		vmaFlushAllocation(m_allocatorRef, staging.GetAllocation(), 0, VK_WHOLE_SIZE);

		// One-time command buffer.
		const VkCommandBufferAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(device, &allocInfo, &cmd);

		const VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(cmd, &beginInfo);

		const VkBufferCopy region{ .srcOffset = 0, .dstOffset = dstOffset, .size = bytes };
		vkCmdCopyBuffer(cmd, staging.Get(), m_buffer.Get(), 1, &region);

		vkEndCommandBuffer(cmd);

		const VkSubmitInfo submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
		};
		vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(queue);
		vkFreeCommandBuffers(device, pool, 1, &cmd);
	}
} // namespace aether
