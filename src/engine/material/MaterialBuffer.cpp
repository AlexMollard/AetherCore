#include "material/MaterialBuffer.hpp"

#include <cstring>
#include <stdexcept>

#include "utils/Assert.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	MaterialBuffer::~MaterialBuffer()
	{
		Shutdown();
	}

	void MaterialBuffer::Initialize(const VulkanContext& ctx)
	{
		m_device = static_cast<gpu::Device>(ctx.GetDevice().device);
		m_allocator = static_cast<gpu::Allocator>(ctx.GetAllocator());

		AE_EXPECT_OR_THROW(buf, UniqueBuffer::CreateMapped(m_allocator, m_device, sizeof(GpuMaterial) * kMaxMaterials, gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress, "MaterialBuffer"));
		m_buffer = std::move(buf);

		const auto& allocInfo = m_buffer.GetAllocationInfo();
		m_mapped = static_cast<GpuMaterial*>(allocInfo.pMappedData);

		m_address = m_buffer.GetDeviceAddress();

		m_freeSlots.reserve(kMaxMaterials);
		for (std::uint32_t i = kMaxMaterials; i-- > 0;)
		{
			m_freeSlots.push_back(i);
		}
	}

	void MaterialBuffer::Shutdown()
	{
		if (m_device == nullptr)
		{
			return;
		}

		m_buffer.Reset();

		m_mapped = nullptr;
		m_address = 0;
		m_device = nullptr;
		m_allocator = nullptr;
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
		AE_EXPECT_OR_THROW_VOID(m_buffer.FlushMapped(static_cast<gpu::DeviceSize>(slot) * sizeof(GpuMaterial), sizeof(GpuMaterial)));
	}
} // namespace aether
