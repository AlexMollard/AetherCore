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
		(void) ctx;

		const gpu::MappedBufferDesc desc{
		        .size = sizeof(GpuMaterial) * kMaxMaterials,
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "MaterialBuffer",
		};
		m_handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_handle.IsValid())
		{
			Throw(AetherError::Engine("MaterialBuffer: CreateMappedBuffer failed"));
		}

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_handle);
		m_mapped = static_cast<GpuMaterial*>(view.mappedPtr);
		m_address = view.deviceAddress;

		m_freeSlots.reserve(kMaxMaterials);
		for (std::uint32_t i = kMaxMaterials; i-- > 0;)
		{
			m_freeSlots.push_back(i);
		}
	}

	void MaterialBuffer::Shutdown()
	{
		if (!m_handle.IsValid())
		{
			return;
		}

		gpu::ResourceRegistry::Destroy(m_handle);
		m_handle = {};
		m_mapped = nullptr;
		m_address = 0;
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
		gpu::ResourceRegistry::FlushMappedBuffer(m_handle, static_cast<gpu::DeviceSize>(slot) * sizeof(GpuMaterial), sizeof(GpuMaterial));
	}
} // namespace aether
