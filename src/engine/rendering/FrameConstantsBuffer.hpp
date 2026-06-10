#pragma once

#include <array>
#include <cstdint>
#include <vk_mem_alloc.h>

#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstants.hpp"
#include "vulkan/UniqueBuffer.hpp"

namespace aether
{
	class VulkanContext;

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
			UniqueBuffer buffer;
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		std::array<PerFrame, kFrameCount> m_frames{};
	};
} // namespace aether
