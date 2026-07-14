#include "material/MaterialBuffer.hpp"

#include <cstring>
#include <stdexcept>

#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	MaterialBuffer::~MaterialBuffer()
	{
		Shutdown();
	}

	void MaterialBuffer::Initialize()
	{
		AE_PROFILE_ZONE();
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

		m_slotAllocator.Reset(kMaxMaterials);
	}

	void MaterialBuffer::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (!m_handle.IsValid())
		{
			return;
		}

		gpu::ResourceRegistry::Destroy(m_handle);
		m_handle = {};
		m_mapped = nullptr;
		m_address = 0;
		m_slotAllocator.Clear();
	}

	std::uint32_t MaterialBuffer::AllocateSlot()
	{
		AE_PROFILE_ZONE();
		const std::scoped_lock lock(m_mutex);
		return m_slotAllocator.Allocate();
	}

	void MaterialBuffer::FreeSlot(std::uint32_t slot)
	{
		if (slot >= kMaxMaterials)
		{
			return;
		}
		const std::scoped_lock lock(m_mutex);
		m_slotAllocator.Free(slot);
	}

	void MaterialBuffer::AdvanceFrame(std::uint64_t frameIndex)
	{
		const std::scoped_lock lock(m_mutex);
		m_slotAllocator.AdvanceFrame(frameIndex);
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
