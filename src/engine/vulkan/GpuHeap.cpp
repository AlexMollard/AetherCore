#include "vulkan/GpuHeap.hpp"

#include <cassert>
#include <cstring>

#include "gpu/CommandList.hpp"

#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/GpuMemoryTracker.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "vulkan/TransferManager.hpp"

namespace aether
{
	void GpuHeap::Initialize(const VulkanContext& ctx, Desc desc)
	{
		AE_PROFILE_ZONE();
		m_allocatorRef = ctx.GetAllocator();
		m_deviceRef = ctx.GetDevice().device;

		constexpr gpu::BufferUsage kBaseUsage = gpu::BufferUsage::Storage | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		const VkBufferUsageFlags2 vkUsage = gpu::ToVk(kBaseUsage | desc.additionalUsage);

		const VkBufferUsageFlags2CreateInfo usageFlags2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
		        .usage = vkUsage,
		};

		// Uploads run on the transfer queue while graphics/compute consume the heap, so
		// the buffer is shared CONCURRENT across those families when they differ - the
		// transfer timeline wait then orders and publishes the copies with no
		// queue-family ownership transfer barriers. Buffers pay no measurable cost for
		// concurrent sharing (it mainly affects image compression).
		std::uint32_t sharedFamilies[3] = {};
		std::uint32_t sharedFamilyCount = 0;
		for (const std::uint32_t family: {ctx.GetGraphicsQueueFamily(), ctx.GetComputeQueueFamily(), ctx.GetTransferQueueFamily()})
		{
			bool known = false;
			for (std::uint32_t i = 0; i < sharedFamilyCount; ++i)
			{
				known = known || sharedFamilies[i] == family;
			}
			if (!known)
			{
				sharedFamilies[sharedFamilyCount++] = family;
			}
		}

		VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .pNext = &usageFlags2,
		        .size = desc.capacityBytes,
		        .usage = 0,
		};
		if (sharedFamilyCount > 1)
		{
			bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
			bufferInfo.queueFamilyIndexCount = sharedFamilyCount;
			bufferInfo.pQueueFamilyIndices = sharedFamilies;
		}
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
		AE_PROFILE_ZONE();
		if (m_memoryTracker != nullptr && m_baseAddress != 0)
		{
			m_memoryTracker->UnregisterRange(m_baseAddress, m_capacityBytes);
		}
		if (m_virtualBlock != VK_NULL_HANDLE)
		{
			for (const auto& [addr, allocation]: m_allocations)
			{
				(void) addr;
				if (allocation != VK_NULL_HANDLE)
				{
					vmaVirtualFree(m_virtualBlock, allocation);
				}
			}
			m_allocations.clear();
			vmaDestroyVirtualBlock(m_virtualBlock);
			m_virtualBlock = VK_NULL_HANDLE;
		}
		else
		{
			m_allocations.clear();
		}
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

	std::uint64_t GpuHeap::UploadBytes(gpu::DeviceAddress dstAddr, const void* src, VkDeviceSize bytes, vulkan::TransferManager& transfer)
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

		// Non-blocking: the copy runs on the transfer queue; the frame submission's
		// timeline wait orders it before any GPU consumption, and the staging buffer is
		// reclaimed once the ticket completes.
		VmaAllocator allocator = m_allocatorRef;
		VkBuffer dstBuffer = m_buffer;
		return transfer.Submit(
		        [stagingBuffer, dstBuffer, dstOffset, bytes](gpu::CommandList& cmdList)
		        {
			        cmdList.CopyBuffer(static_cast<void*>(stagingBuffer), static_cast<void*>(dstBuffer), 0, dstOffset, bytes);
		        },
		        [allocator, stagingBuffer, stagingAllocation]() { vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation); });
	}
} // namespace aether
