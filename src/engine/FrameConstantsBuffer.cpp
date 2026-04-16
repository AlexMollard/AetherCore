#include "FrameConstantsBuffer.hpp"

#include <cstring>
#include <format>

#include "AetherExceptions.hpp"
#include "VulkanContext.hpp"

namespace aether
{
	FrameConstantsBuffer::~FrameConstantsBuffer()
	{
		Shutdown();
	}

	void FrameConstantsBuffer::Initialize(const VulkanContext& ctx)
	{
		m_device = ctx.GetDevice().device;
		m_allocator = ctx.GetAllocator();

		// Create persistently-mapped host-visible buffers with BDA support.
		const VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = sizeof(FrameConstants),
			.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		};
		const VmaAllocationCreateInfo allocInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO,
		};

		for (std::uint32_t i = 0; i < kFrameCount; ++i)
		{
			VmaAllocationInfo outInfo{};
			if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &m_frames[i].buffer, &m_frames[i].allocation, &outInfo) != VK_SUCCESS)
			{
				throw VulkanError(std::format("Failed to create FrameConstants buffer (frame {}).", i));
			}
			m_frames[i].mapped = outInfo.pMappedData;

			const VkBufferDeviceAddressInfo addrInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				.buffer = m_frames[i].buffer,
			};
			m_frames[i].address = vkGetBufferDeviceAddress(m_device, &addrInfo);
		}
	}

	void FrameConstantsBuffer::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}

		for (auto& frame: m_frames)
		{
			if (frame.buffer != VK_NULL_HANDLE)
			{
				vmaDestroyBuffer(m_allocator, frame.buffer, frame.allocation);
				frame.buffer = VK_NULL_HANDLE;
				frame.allocation = VK_NULL_HANDLE;
				frame.mapped = nullptr;
				frame.address = 0;
			}
		}

		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

	void FrameConstantsBuffer::Write(std::uint32_t frameIndex, const FrameConstants& data)
	{
		std::memcpy(m_frames[frameIndex].mapped, &data, sizeof(FrameConstants));
		vmaFlushAllocation(m_allocator, m_frames[frameIndex].allocation, 0, VK_WHOLE_SIZE);
	}

	VkDeviceAddress FrameConstantsBuffer::GetDeviceAddress(std::uint32_t frameIndex) const
	{
		return m_frames[frameIndex].address;
	}
} // namespace aether
