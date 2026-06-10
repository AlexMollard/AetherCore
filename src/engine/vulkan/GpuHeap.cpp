#include "vulkan/GpuHeap.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void GpuHeap::Initialize(const VulkanContext& ctx, Desc desc)
	{
		m_allocatorRef = ctx.GetAllocator();
		m_deviceRef = ctx.GetDevice().device;

		constexpr VkBufferUsageFlags kBaseUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		AE_EXPECT_OR_THROW(buffer, UniqueBuffer::CreateDeviceLocal(m_allocatorRef, m_deviceRef, desc.capacityBytes, kBaseUsage | desc.additionalUsage, desc.debugName));
		m_buffer = std::move(buffer);
		m_freeList.push_back({0, desc.capacityBytes});
	}

	void GpuHeap::Shutdown()
	{
		m_buffer.Reset();
		m_freeList.clear();
	}

	VkDeviceSize GpuHeap::AllocBytes(VkDeviceSize bytes)
	{
		constexpr VkDeviceSize kMinAlignment = 16;
		bytes = (bytes + kMinAlignment - 1) & ~(kMinAlignment - 1);

		for (std::size_t idx = 0; idx < m_freeList.size(); ++idx)
		{
			FreeBlock& blk = m_freeList[idx];

			const VkDeviceSize alignedOffset = (blk.offset + kMinAlignment - 1) & ~(kMinAlignment - 1);
			const VkDeviceSize waste = alignedOffset - blk.offset;
			if (waste >= blk.size)
			{
				continue;
			}
			const VkDeviceSize effectiveSize = blk.size - waste;

			if (effectiveSize < bytes)
			{
				continue;
			}

			// Split off alignment waste as a separate free block (reuse current slot).
			if (waste > 0)
			{
				m_freeList.insert(m_freeList.begin() + static_cast<std::ptrdiff_t>(idx) + 1, FreeBlock{alignedOffset, effectiveSize});
				m_freeList[idx].size = waste;
				++idx;
			}

			// Split off remaining free space after the allocation.
			if (effectiveSize > bytes)
			{
				m_freeList.insert(m_freeList.begin() + static_cast<std::ptrdiff_t>(idx) + 1, FreeBlock{alignedOffset + bytes, effectiveSize - bytes});
				// Block at idx now represents the allocated region: {alignedOffset, bytes}
				// Replace it with the remainder since we only split once - keep it simple:
				// erase the usable slot (idx) since we'll let the normal free-list manage leftovers.
				m_freeList.erase(m_freeList.begin() + static_cast<std::ptrdiff_t>(idx));
			}
			else
			{
				// Exact fit: just erase the usable block.
				m_freeList.erase(m_freeList.begin() + static_cast<std::ptrdiff_t>(idx));
			}

			return alignedOffset;
		}
		return kInvalidOffset;
	}

	void GpuHeap::FreeBytes(VkDeviceSize offset, VkDeviceSize bytes)
	{
		// Round up to match the alignment applied by AllocBytes.
		constexpr VkDeviceSize kMinAlignment = 16;
		bytes = (bytes + kMinAlignment - 1) & ~(kMinAlignment - 1);
		auto it = std::lower_bound(m_freeList.begin(), m_freeList.end(), offset, [](const FreeBlock& b, VkDeviceSize o) { return b.offset < o; });
		it = m_freeList.insert(it, {offset, bytes});

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

	void GpuHeap::UploadBytes(gpu::DeviceAddress dstAddr, const void* src, VkDeviceSize bytes, VkDevice device, VkQueue queue, VkCommandPool pool)
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

		const VkBufferCopy region{.srcOffset = 0, .dstOffset = dstOffset, .size = bytes};
		vkCmdCopyBuffer(cmd, staging.Get(), m_buffer.Get(), 1, &region);

		const VkResult endResult = vkEndCommandBuffer(cmd);
		if (endResult != VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, pool, 1, &cmd);
			Throw(AetherError::Vulkan(static_cast<int32_t>(endResult), "GpuHeap: failed to end upload command buffer"));
		}

		const VkCommandBufferSubmitInfo cbInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = cmd,
		};
		const VkSubmitInfo2 submitInfo{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cbInfo,
		};
		const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence fence = VK_NULL_HANDLE;
		if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
		{
			vkFreeCommandBuffers(device, pool, 1, &cmd);
			Throw(AetherError::Vulkan(0, "GpuHeap: failed to create upload fence"));
		}
		const VkResult submitResult = vkQueueSubmit2(queue, 1, &submitInfo, fence);
		if (submitResult != VK_SUCCESS)
		{
			vkDestroyFence(device, fence, nullptr);
			vkFreeCommandBuffers(device, pool, 1, &cmd);
			Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "GpuHeap: failed to submit upload command buffer"));
		}
		(void) vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkDestroyFence(device, fence, nullptr);
		vkFreeCommandBuffers(device, pool, 1, &cmd);
	}
} // namespace aether
