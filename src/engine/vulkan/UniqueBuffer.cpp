#include "vulkan/UniqueBuffer.hpp"

#include <format>
#include <utility>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "vulkan/GpuEnumConversions.hpp"

namespace aether
{
	// Engine-side forwarders: cast opaque gpu:: types to Vk* and delegate
	// to the Vulkan-internal overload. Keeps the bridge in one place.

	Expected<UniqueBuffer> UniqueBuffer::CreateMapped(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, gpu::BufferUsage usage, const char* debugName)
	{
		return CreateMapped(static_cast<VmaAllocator>(allocator), static_cast<VkDevice>(device), static_cast<VkDeviceSize>(size), gpu::ToVk(usage), debugName);
	}

	Expected<UniqueBuffer> UniqueBuffer::CreateMapped(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, gpu::BufferUsage usage, gpu::MappedMemoryUsage memoryUsage, const char* debugName)
	{
		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = static_cast<VkDeviceSize>(size),
		        .usage = gpu::ToVk(usage),
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		const VmaAllocationCreateInfo allocInfo{
		        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		        .usage = VMA_MEMORY_USAGE_AUTO,
		};
		return Create(static_cast<VmaAllocator>(allocator), static_cast<VkDevice>(device), bufferInfo, allocInfo);
	}

	Expected<UniqueBuffer> UniqueBuffer::CreateDeviceLocal(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, gpu::BufferUsage usage, const char* debugName)
	{
		return CreateDeviceLocal(static_cast<VmaAllocator>(allocator), static_cast<VkDevice>(device), static_cast<VkDeviceSize>(size), gpu::ToVk(usage), debugName);
	}

	UniqueBuffer::~UniqueBuffer()
	{
		Reset();
	}

	UniqueBuffer::UniqueBuffer(UniqueBuffer&& other) noexcept
	      : m_allocator(std::exchange(other.m_allocator, VK_NULL_HANDLE)),
	        m_device(std::exchange(other.m_device, VK_NULL_HANDLE)),
	        m_buffer(std::exchange(other.m_buffer, VK_NULL_HANDLE)),
	        m_allocation(std::exchange(other.m_allocation, VK_NULL_HANDLE)),
	        m_allocationInfo(other.m_allocationInfo),
	        m_ownsAllocation(std::exchange(other.m_ownsAllocation, true)),
	        m_usage(other.m_usage),
	        m_size(other.m_size),
	        m_deviceAddress(other.m_deviceAddress),
	        m_virtualResourceId(other.m_virtualResourceId)
	{
		other.m_allocationInfo = {};
		other.m_usage = 0;
		other.m_size = 0;
		other.m_deviceAddress = 0;
		other.m_virtualResourceId = 0;
	}

	UniqueBuffer& UniqueBuffer::operator=(UniqueBuffer&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		Reset();

		m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
		m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
		m_buffer = std::exchange(other.m_buffer, VK_NULL_HANDLE);
		m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
		m_allocationInfo = other.m_allocationInfo;
		m_ownsAllocation = std::exchange(other.m_ownsAllocation, true);
		m_usage = other.m_usage;
		m_size = other.m_size;
		m_deviceAddress = other.m_deviceAddress;
		m_virtualResourceId = other.m_virtualResourceId;

		other.m_allocationInfo = {};
		other.m_usage = 0;
		other.m_size = 0;
		other.m_deviceAddress = 0;
		other.m_virtualResourceId = 0;

		return *this;
	}

	Expected<UniqueBuffer> UniqueBuffer::Create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& bufferCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo)
	{
		UniqueBuffer out;
		out.m_allocator = allocator;
		out.m_device = device;
		out.m_usage = bufferCreateInfo.usage;
		out.m_size = bufferCreateInfo.size;

		const VkResult createResult = vmaCreateBuffer(allocator, &bufferCreateInfo, &allocationCreateInfo, &out.m_buffer, &out.m_allocation, &out.m_allocationInfo);

		if (createResult != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(createResult), "Failed to create VMA buffer"));
		}

		if ((out.m_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
		{
			const VkBufferDeviceAddressInfo addressInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .pNext = nullptr,
			        .buffer = out.m_buffer,
			};
			out.m_deviceAddress = vkGetBufferDeviceAddress(device, &addressInfo);
		}

		return out;
	}

	Expected<UniqueBuffer> UniqueBuffer::CreateAliased(VkDevice device, VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset)
	{
		UniqueBuffer out;
		out.m_allocator = allocator;
		out.m_device = device;
		out.m_ownsAllocation = false;
		out.m_usage = usage;
		out.m_size = size;

		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = size,
		        .usage = usage,
		};
		VkResult result = vkCreateBuffer(device, &bufInfo, nullptr, &out.m_buffer);
		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "UniqueBuffer::CreateAliased: vkCreateBuffer failed"));
		}

		VmaAllocationInfo existingAllocInfo;
		vmaGetAllocationInfo(allocator, existingAllocation, &existingAllocInfo);

		const VkBindBufferMemoryInfo bindInfo{
		        .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
		        .buffer = out.m_buffer,
		        .memory = existingAllocInfo.deviceMemory,
		        .memoryOffset = memoryOffset,
		};
		result = vkBindBufferMemory2(device, 1, &bindInfo);
		if (result != VK_SUCCESS)
		{
			vkDestroyBuffer(device, out.m_buffer, nullptr);
			out.m_buffer = VK_NULL_HANDLE;
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "UniqueBuffer::CreateAliased: vkBindBufferMemory2 failed"));
		}

		out.m_allocation = existingAllocation;

		if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
		{
			const VkBufferDeviceAddressInfo addressInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .buffer = out.m_buffer,
			};
			vkGetBufferDeviceAddress(device, &addressInfo);
		}

		return out;
	}

	Expected<UniqueBuffer> UniqueBuffer::CreateStorageBuffer(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize size, const char* debugName)
	{
		VkBufferCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = static_cast<VkDeviceSize>(size),
		        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		};
		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		AE_EXPECT_OR_THROW(out, Create(static_cast<VmaAllocator>(allocator), static_cast<VkDevice>(device), info, allocInfo));
		if (debugName != nullptr)
		{
			out.SetName(debugName);
		}
		return out;
	}

	Expected<UniqueBuffer> UniqueBuffer::CreateMapped(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName)
	{
		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = size,
		        .usage = usage,
		};
		const VmaAllocationCreateInfo allocInfo{
		        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		        .usage = VMA_MEMORY_USAGE_AUTO,
		};
		auto result = Create(allocator, device, bufInfo, allocInfo);
		if (result && debugName)
		{
			result->SetName(debugName);
		}
		return result;
	}

	Expected<UniqueBuffer> UniqueBuffer::CreateDeviceLocal(VmaAllocator allocator, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName)
	{
		const VkBufferCreateInfo bufInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = size,
		        .usage = usage,
		};
		const VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		};
		auto result = Create(allocator, device, bufInfo, allocInfo);
		if (result && debugName)
		{
			result->SetName(debugName);
		}
		return result;
	}

	void UniqueBuffer::SetName(const char* name) const
	{
		if (!s_setObjectNameFn || m_buffer == VK_NULL_HANDLE || !name)
		{
			return;
		}
		const VkDebugUtilsObjectNameInfoEXT info{
		        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		        .objectType = VK_OBJECT_TYPE_BUFFER,
		        .objectHandle = reinterpret_cast<std::uint64_t>(m_buffer),
		        .pObjectName = name,
		};
		s_setObjectNameFn(m_device, &info);
	}

	void UniqueBuffer::SetObjectNameFunction(PFN_vkSetDebugUtilsObjectNameEXT fn)
	{
		s_setObjectNameFn = fn;
	}

	void UniqueBuffer::Reset()
	{
		if (m_buffer != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE)
		{
			if (m_ownsAllocation && m_allocation != VK_NULL_HANDLE)
			{
				vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
			}
			else
			{
				VmaAllocatorInfo allocInfo;
				vmaGetAllocatorInfo(m_allocator, &allocInfo);
				vkDestroyBuffer(allocInfo.device, m_buffer, nullptr);
			}
		}

		m_buffer = VK_NULL_HANDLE;
		m_allocation = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
		m_allocationInfo = {};
		m_ownsAllocation = true;
		m_usage = 0;
		m_size = 0;
		m_deviceAddress = 0;
		m_virtualResourceId = 0;
	}

	Expected<void> UniqueBuffer::FlushMapped(gpu::DeviceSize offset, gpu::DeviceSize size) const
	{
		const VkResult result = vmaFlushAllocation(m_allocator, m_allocation, static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(size));
		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "vmaFlushAllocation failed"));
		}
		return {};
	}

	VkBuffer UniqueBuffer::Get() const
	{
		return m_buffer;
	}

	VmaAllocation UniqueBuffer::GetAllocation() const
	{
		return m_allocation;
	}

	VmaAllocator UniqueBuffer::GetAllocator() const
	{
		return m_allocator;
	}

	const VmaAllocationInfo& UniqueBuffer::GetAllocationInfo() const
	{
		return m_allocationInfo;
	}

	gpu::DeviceAddress UniqueBuffer::GetDeviceAddress() const
	{
		return m_deviceAddress;
	}

	VkBufferUsageFlags UniqueBuffer::GetUsage() const
	{
		return m_usage;
	}

	VkDeviceSize UniqueBuffer::GetSize() const
	{
		return m_size;
	}

	bool UniqueBuffer::HasDeviceAddress() const
	{
		return m_deviceAddress != 0;
	}

	std::uint64_t UniqueBuffer::GetVirtualResourceId() const
	{
		return m_virtualResourceId;
	}

	void UniqueBuffer::SetVirtualResourceId(const std::uint64_t virtualResourceId)
	{
		m_virtualResourceId = virtualResourceId;
	}

	UniqueBuffer::operator bool() const
	{
		return m_buffer != VK_NULL_HANDLE;
	}
} // namespace aether
