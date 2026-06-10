#include "rendering/FrameConstantsBuffer.hpp"

#include <cstring>
#include <format>

#include "utils/Assert.hpp"
#include "vulkan/VulkanContext.hpp"

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
			AE_EXPECT_OR_THROW(buf, UniqueBuffer::Create(m_allocator, m_device, bufferInfo, allocInfo));
			m_frames[i].buffer = std::move(buf);
			m_frames[i].mapped = m_frames[i].buffer.GetAllocationInfo().pMappedData;

			const VkBufferDeviceAddressInfo addrInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			        .buffer = m_frames[i].buffer.Get(),
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
			frame.buffer.Reset();
			frame.mapped = nullptr;
			frame.address = 0;
		}

		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

	void FrameConstantsBuffer::Write(std::uint32_t frameIndex, const FrameConstants& data)
	{
		std::memcpy(m_frames[frameIndex].mapped, &data, sizeof(FrameConstants));
		AE_EXPECT_OR_THROW_VOID(m_frames[frameIndex].buffer.FlushMapped());
	}

	gpu::DeviceAddress FrameConstantsBuffer::GetDeviceAddress(std::uint32_t frameIndex) const
	{
		return m_frames[frameIndex].address;
	}

	std::uint64_t FrameConstantsBuffer::GetDeviceAddressU64(std::uint32_t frameIndex) const
	{
		return static_cast<std::uint64_t>(m_frames[frameIndex].address);
	}
} // namespace aether
