#include "UniqueBuffer.hpp"

#include <format>
#include <utility>

#include "AetherExceptions.hpp"

namespace aether
{
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

	UniqueBuffer UniqueBuffer::Create(VmaAllocator allocator, VkDevice device, const VkBufferCreateInfo& bufferCreateInfo, const VmaAllocationCreateInfo& allocationCreateInfo)
	{
		UniqueBuffer out;
		out.m_allocator = allocator;
		out.m_device = device;
		out.m_usage = bufferCreateInfo.usage;
		out.m_size = bufferCreateInfo.size;

		const VkResult createResult = vmaCreateBuffer(allocator, &bufferCreateInfo, &allocationCreateInfo, &out.m_buffer, &out.m_allocation, &out.m_allocationInfo);

		if (createResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to create VMA buffer. VkResult={}", static_cast<int>(createResult)));
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

	void UniqueBuffer::Reset()
	{
		if (m_buffer != VK_NULL_HANDLE && m_allocation != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
		}

		m_buffer = VK_NULL_HANDLE;
		m_allocation = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
		m_allocationInfo = {};
		m_usage = 0;
		m_size = 0;
		m_deviceAddress = 0;
		m_virtualResourceId = 0;
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

	VkDeviceAddress UniqueBuffer::GetDeviceAddress() const
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
