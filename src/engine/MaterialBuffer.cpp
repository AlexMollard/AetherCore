#include "MaterialBuffer.hpp"

#include <cstring>
#include <stdexcept>

#include "AetherExceptions.hpp"
#include "VulkanContext.hpp"

namespace aether
{
	MaterialBuffer::~MaterialBuffer()
	{
		Shutdown();
	}

	void MaterialBuffer::Initialize(const VulkanContext& ctx)
	{
		m_device = ctx.GetDevice().device;
		m_allocator = ctx.GetAllocator();

		const VkBufferCreateInfo bufferInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = sizeof(GpuMaterial) * kMaxMaterials,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		};
		const VmaAllocationCreateInfo allocInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO,
		};

		VmaAllocationInfo outInfo{};
		if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &m_buffer, &m_allocation, &outInfo) != VK_SUCCESS)
		{
			throw VulkanError("Failed to create MaterialBuffer GPU buffer.");
		}

		m_mapped = static_cast<GpuMaterial*>(outInfo.pMappedData);

		const VkBufferDeviceAddressInfo addrInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = m_buffer,
		};
		m_address = vkGetBufferDeviceAddress(m_device, &addrInfo);

		// Seed the free-slot list (high-to-low so slot 0 is returned first).
		m_freeSlots.reserve(kMaxMaterials);
		for (std::uint32_t i = kMaxMaterials; i-- > 0;)
		{
			m_freeSlots.push_back(i);
		}
	}

	void MaterialBuffer::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}

		if (m_buffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
			m_buffer = VK_NULL_HANDLE;
		}

		m_mapped = nullptr;
		m_address = 0;
		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
		m_freeSlots.clear();
	}

	std::uint32_t MaterialBuffer::AllocateSlot()
	{
		std::scoped_lock lock(m_mutex);
		if (m_freeSlots.empty())
		{
			return kInvalidSlot;
		}
		const std::uint32_t slot = m_freeSlots.back();
		m_freeSlots.pop_back();
		return slot;
	}

	void MaterialBuffer::FreeSlot(std::uint32_t slot)
	{
		if (slot >= kMaxMaterials)
		{
			return;
		}
		std::scoped_lock lock(m_mutex);
		m_freeSlots.push_back(slot);
	}

	void MaterialBuffer::Write(std::uint32_t slot, const GpuMaterial& material)
	{
		if (slot >= kMaxMaterials || m_mapped == nullptr)
		{
			return;
		}
		m_mapped[slot] = material;
	}
} // namespace aether
