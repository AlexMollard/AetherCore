#include "material/EffectParamBuffer.hpp"

#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	EffectParamBuffer::~EffectParamBuffer()
	{
		Shutdown();
	}

	void EffectParamBuffer::Initialize()
	{
		AE_PROFILE_ZONE();
		const gpu::MappedBufferDesc desc{
		        .size = sizeof(EffectParams) * kMaxEffects,
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "EffectParamBuffer",
		};
		m_handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_handle.IsValid())
		{
			Throw(AetherError::Engine("EffectParamBuffer: CreateMappedBuffer failed"));
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_handle);
		m_mapped = static_cast<EffectParams*>(view.mappedPtr);
		m_address = view.deviceAddress;
		m_slotAllocator.Reset(kMaxEffects);
	}

	void EffectParamBuffer::Shutdown()
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

	std::uint32_t EffectParamBuffer::AllocateSlot()
	{
		std::scoped_lock lock(m_mutex);
		return m_slotAllocator.Allocate();
	}

	void EffectParamBuffer::FreeSlot(std::uint32_t slot)
	{
		if (slot >= kMaxEffects)
		{
			return;
		}
		std::scoped_lock lock(m_mutex);
		m_slotAllocator.Free(slot);
	}

	void EffectParamBuffer::AdvanceFrame(std::uint64_t frameIndex)
	{
		std::scoped_lock lock(m_mutex);
		m_slotAllocator.AdvanceFrame(frameIndex);
	}

	void EffectParamBuffer::Write(std::uint32_t slot, const EffectParams& params)
	{
		if (slot >= kMaxEffects || m_mapped == nullptr)
		{
			return;
		}
		m_mapped[slot] = params;
		gpu::ResourceRegistry::FlushMappedBuffer(m_handle, static_cast<gpu::DeviceSize>(slot) * sizeof(EffectParams), sizeof(EffectParams));
	}
} // namespace aether
