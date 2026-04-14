#pragma once

#include <array>
#include <cstdint>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "FrameConstants.hpp"
#include "Swapchain.hpp"

namespace aether
{
	class VulkanContext;

	// Manages a per-frame uniform buffer for FrameConstants (double-buffered to match
	// kMaxFramesInFlight). Accessed exclusively via Buffer Device Address pushed in the
	// per-draw push constants — no descriptor set required.
	class FrameConstantsBuffer
	{
	public:
		FrameConstantsBuffer() = default;
		~FrameConstantsBuffer();

		FrameConstantsBuffer(const FrameConstantsBuffer&) = delete;
		FrameConstantsBuffer& operator=(const FrameConstantsBuffer&) = delete;

		void Initialize(const VulkanContext& ctx);
		void Shutdown();

		// Write per-frame data into the mapped host-visible buffer for the given frame slot.
		void Write(std::uint32_t frameIndex, const FrameConstants& data);

		// Returns the Vulkan Buffer Device Address for the given frame slot.
		// Push this directly into the push constant block; shaders dereference it via
		// [[vk::buffer_reference]].
		[[nodiscard]] VkDeviceAddress GetDeviceAddress(std::uint32_t frameIndex) const;

	private:
		static constexpr std::uint32_t kFrameCount = Swapchain::kMaxFramesInFlight;

		struct PerFrame
		{
			VkBuffer        buffer = VK_NULL_HANDLE;
			VmaAllocation   allocation = VK_NULL_HANDLE;
			void* mapped = nullptr;
			VkDeviceAddress address = 0;
		};

		VkDevice     m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		std::array<PerFrame, kFrameCount> m_frames{};
	};
}
