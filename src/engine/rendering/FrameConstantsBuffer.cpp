#include "rendering/FrameConstantsBuffer.hpp"

#include <cstring>
#include <format>

#include "utils/Assert.hpp"

namespace aether
{
	FrameConstantsBuffer::~FrameConstantsBuffer()
	{
		Shutdown();
	}

	void FrameConstantsBuffer::Initialize()
	{
		for (std::uint32_t i = 0; i < kFrameCount; ++i)
		{
			const gpu::MappedBufferDesc desc{
			        .size = sizeof(FrameConstants),
			        .usage = gpu::BufferUsage::Uniform | gpu::BufferUsage::ShaderDeviceAddress,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = "FrameConstants",
			};
			m_frames[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!m_frames[i].handle.IsValid())
			{
				Throw(AetherError::Engine("FrameConstantsBuffer: CreateMappedBuffer failed"));
			}
			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_frames[i].handle);
			m_frames[i].mapped = view.mappedPtr;
			m_frames[i].address = view.deviceAddress;
		}
		m_initialized = true;
	}

	void FrameConstantsBuffer::Shutdown()
	{
		if (!m_initialized)
		{
			return;
		}

		for (auto& frame: m_frames)
		{
			if (frame.handle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(frame.handle);
			}
			frame.handle = {};
			frame.mapped = nullptr;
			frame.address = 0;
		}

		m_initialized = false;
	}

	void FrameConstantsBuffer::Write(std::uint32_t frameIndex, const FrameConstants& data)
	{
		std::memcpy(m_frames[frameIndex].mapped, &data, sizeof(FrameConstants));
		gpu::ResourceRegistry::FlushMappedBuffer(m_frames[frameIndex].handle, 0, sizeof(FrameConstants));
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
