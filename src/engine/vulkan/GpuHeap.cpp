#include "vulkan/GpuHeap.hpp"

#include <cassert>
#include <cstring>

#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/GpuMemoryTracker.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	void GpuHeap::Initialize(const VulkanContext& ctx, Desc desc)
	{
		m_allocatorRef = ctx.GetAllocator();
		m_deviceRef = ctx.GetDevice().device;

		constexpr gpu::BufferUsage kBaseUsage = gpu::BufferUsage::Storage | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		const VkBufferUsageFlags2 vkUsage = gpu::ToVk(kBaseUsage | desc.additionalUsage);

		const VkBufferUsageFlags2CreateInfo usageFlags2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .usage = vkUsage,
		};

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &usageFlags2,
		        .size = desc.capacityBytes,
		        .usage = 0,
		};
		const VmaAllocationCreateInfo allocInfo{
		        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
		        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		};
		VmaAllocationInfo vmaInfo{};
		if (vmaCreateBuffer(m_allocatorRef, &bufferInfo, &allocInfo, &m_buffer, &m_bufferAllocation, &vmaInfo) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "GpuHeap: failed to create device-local buffer"));
		}

		const VkBufferDeviceAddressInfo addrInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		        .buffer = m_buffer,
		};
		m_baseAddress = vkGetBufferDeviceAddress(m_deviceRef, &addrInfo);
		m_capacityBytes = desc.capacityBytes;
		if (desc.debugName != nullptr)
		{
			m_debugName = desc.debugName;
			vkutil::SetObjectName(m_deviceRef, reinterpret_cast<std::uint64_t>(m_buffer), VK_OBJECT_TYPE_BUFFER, desc.debugName);
		}
		else
		{
			m_debugName = "<gpu_heap>";
		}

		if (m_memoryTracker != nullptr && m_baseAddress != 0)
		{
			m_memoryTracker->Register(m_baseAddress, m_capacityBytes, m_debugName, GpuMemoryTracker::ResourceType::GpuHeap);
		}

		const VmaVirtualBlockCreateInfo blockInfo{
		        .size = desc.capacityBytes,
		};
		if (vmaCreateVirtualBlock(&blockInfo, &m_virtualBlock) != VK_SUCCESS)
		{
			vmaDestroyBuffer(m_allocatorRef, m_buffer, m_bufferAllocation);
			m_buffer = VK_NULL_HANDLE;
			m_bufferAllocation = VK_NULL_HANDLE;
			Throw(AetherError::Vulkan(0, "GpuHeap: failed to create VmaVirtualBlock"));
		}
	}

	void GpuHeap::Shutdown()
	{
		if (m_memoryTracker != nullptr && m_baseAddress != 0)
		{
			m_memoryTracker->UnregisterRange(m_baseAddress, m_capacityBytes);
		}
		if (m_virtualBlock != VK_NULL_HANDLE)
		{
			vmaDestroyVirtualBlock(m_virtualBlock);
			m_virtualBlock = VK_NULL_HANDLE;
		}
		m_allocations.clear();
		if (m_buffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_allocatorRef, m_buffer, m_bufferAllocation);
			m_buffer = VK_NULL_HANDLE;
			m_bufferAllocation = VK_NULL_HANDLE;
		}
		m_baseAddress = 0;
		m_capacityBytes = 0;
	}

	void GpuHeap::SetMemoryTracker(GpuMemoryTracker* tracker)
	{
		m_memoryTracker = tracker;
		if (m_memoryTracker != nullptr && m_baseAddress != 0)
		{
			m_memoryTracker->Register(m_baseAddress, m_capacityBytes, m_debugName, GpuMemoryTracker::ResourceType::GpuHeap);
		}
	}

	VkDeviceSize GpuHeap::AllocBytes(VkDeviceSize bytes)
	{
		constexpr VkDeviceSize kMinAlignment = 16;
		bytes = (bytes + kMinAlignment - 1) & ~(kMinAlignment - 1);

		const VmaVirtualAllocationCreateInfo allocInfo{
		        .size = bytes,
		        .alignment = kMinAlignment,
		};
		VmaVirtualAllocation handle = VK_NULL_HANDLE;
		VkDeviceSize offset = VK_WHOLE_SIZE;
		if (vmaVirtualAllocate(m_virtualBlock, &allocInfo, &handle, &offset) != VK_SUCCESS)
		{
			return kInvalidOffset;
		}
		const gpu::DeviceAddress addr = m_baseAddress + offset;
		m_allocations[addr] = handle;
		return offset;
	}

	void GpuHeap::FreeBytes(gpu::DeviceAddress addr)
	{
		const auto it = m_allocations.find(addr);
		if (it == m_allocations.end())
		{
			return;
		}
		vmaVirtualFree(m_virtualBlock, it->second);
		m_allocations.erase(it);
	}

	void GpuHeap::UploadBytes(gpu::DeviceAddress dstAddr, const void* src, VkDeviceSize bytes, VkDevice device, VkQueue queue, VkCommandPool pool)
	{
		assert(dstAddr >= m_baseAddress);
		const auto dstOffset = static_cast<VkDeviceSize>(dstAddr - m_baseAddress);

		const VkBufferUsageFlags2CreateInfo stagingUsageFlags2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .usage = VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT,
		};

		const VkBufferCreateInfo stagingInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &stagingUsageFlags2,
		        .size = bytes,
		        .usage = 0,
		};
		const VmaAllocationCreateInfo stagingAllocInfo{
		        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		        .usage = VMA_MEMORY_USAGE_AUTO,
		};
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VmaAllocation stagingAllocation = VK_NULL_HANDLE;
		VmaAllocationInfo stagingVmaInfo{};
		if (vmaCreateBuffer(m_allocatorRef, &stagingInfo, &stagingAllocInfo, &stagingBuffer, &stagingAllocation, &stagingVmaInfo) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "GpuHeap: failed to create staging buffer"));
		}
		std::memcpy(stagingVmaInfo.pMappedData, src, static_cast<std::size_t>(bytes));
		vmaFlushAllocation(m_allocatorRef, stagingAllocation, 0, bytes);

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
		vkCmdCopyBuffer(cmd, stagingBuffer, m_buffer, 1, &region);

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
		AE_ASSERT_ALWAYS(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "GpuHeap: fence wait failed (device lost?)");
		vkDestroyFence(device, fence, nullptr);
		vkFreeCommandBuffers(device, pool, 1, &cmd);
		vmaDestroyBuffer(m_allocatorRef, stagingBuffer, stagingAllocation);
	}
} // namespace aether
