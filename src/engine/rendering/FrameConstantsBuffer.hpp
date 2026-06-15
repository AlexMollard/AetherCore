#pragma once

#include <array>
#include <cstdint>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/FrameConstants.hpp"

namespace aether
{
	class VulkanContext;

	// Per-frame uniform buffer for the frame constants block.
	//
	// Storage path: stores gpu::BufferHandle per frame in m_frames (8 bytes
	// each, typed, generation-checked). Allocated through
	// gpu::ResourceRegistry::CreateMappedBuffer which uses the registry's
	// 3-frame deferred-destruction ring. CPU writes go through
	// ResolveMappedBuffer().mappedPtr; GPU addresses through
	// ResolveBuffer().deviceAddress; record-time binding through
	// ResolveBufferVkHandle(). No raw VkBuffer is held.
	class FrameConstantsBuffer
	{
	public:
		FrameConstantsBuffer() = default;
		~FrameConstantsBuffer();

		FrameConstantsBuffer(const FrameConstantsBuffer&) = AE_DELETE_MSG("use std::move");
		FrameConstantsBuffer& operator=(const FrameConstantsBuffer&) = AE_DELETE_MSG("use std::move");

		void Initialize(const VulkanContext& ctx);
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
