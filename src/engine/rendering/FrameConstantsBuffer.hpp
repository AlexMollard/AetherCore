#pragma once

#include <array>
#include <cstdint>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/FrameConstants.hpp"

namespace aether
{
	class FrameConstantsBuffer
	{
	public:
		FrameConstantsBuffer() = default;
		~FrameConstantsBuffer();

		FrameConstantsBuffer(const FrameConstantsBuffer&) = AE_DELETE_MSG("use std::move");
		FrameConstantsBuffer& operator=(const FrameConstantsBuffer&) = AE_DELETE_MSG("use std::move");

		void Initialize();
		void Shutdown();

		void Write(std::uint32_t frameIndex, const FrameConstants& data);

		[[nodiscard]] gpu::DeviceAddress GetDeviceAddress(std::uint32_t frameIndex) const;
		[[nodiscard]] std::uint64_t GetDeviceAddressU64(std::uint32_t frameIndex) const;

	private:
		static constexpr std::uint32_t kFrameCount = kMaxFramesInFlight;

		struct PerFrame
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		std::array<PerFrame, kFrameCount> m_frames{};
		bool m_initialized = false;
	};
} // namespace aether
