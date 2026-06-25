#pragma once

#include <cstdint>
#include <vector>
#include <VkBootstrap.h>
#include "vulkan/volk.hpp"

#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/Semaphore.hpp"
#include <vk_mem_alloc.h>
#include "gpu/GpuEnums.hpp"

namespace aether
{
	class VulkanContext;
	class Window;

	class Swapchain
	{
	public:
		static constexpr std::uint32_t kMaxFramesInFlight = 3;

		Swapchain() = default;

		~Swapchain()
		{
			Shutdown(m_device);
		}

		Swapchain(const Swapchain&) = delete;
		Swapchain& operator=(const Swapchain&) = delete;
		Swapchain(Swapchain&&) = delete;
		Swapchain& operator=(Swapchain&&) = delete;

		void Initialize(const VulkanContext& ctx, const Window& window, bool enableVsync);
		void Shutdown(VkDevice device);

		// Acquires next image, resets command pool, begins the command buffer,
		// and performs swapchain-image / depth layout transitions.
		// Sets IsFrameValid() to false if the swapchain is out of date (skip
		// EndFrame).
		void BeginFrame(VkDevice device);

		// Engine-side end-of-frame submission. Transitions to present layout,
		// ends command buffer, submits, and presents. No-op if !IsFrameValid().
		// The opaque `gpu::TimelineSemaphoreHandle` pImpl pointers are cast
		// to `VkSemaphore` here so the engine TU never sees a `Vk*` token.
		void SubmitAndPresent(VkQueue graphicsQueue, VkQueue presentQueue, gpu::TimelineSemaphoreHandle extraWaitSemaphore = nullptr, std::uint64_t extraWaitValue = 0);

		[[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const;
		[[nodiscard]] gpu::Extent2D GetExtent() const;
		[[nodiscard]] gpu::Format GetImageFormat() const;
		[[nodiscard]] gpu::Format GetDepthFormat() const;
		// Per-frame image/view accessors used by RenderGraph::Execute.
		[[nodiscard]] VkImage GetCurrentImage() const;
		[[nodiscard]] VkImageView GetCurrentImageView() const;
		[[nodiscard]] VkImage GetDepthImage() const;
		[[nodiscard]] VkImageView GetDepthImageView() const;
		[[nodiscard]] bool IsFrameValid() const;
		[[nodiscard]] bool NeedsRecreation() const;
		void ClearRecreationFlag();

	private:
		struct FrameSync
		{
			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkSemaphore imageAvailable = VK_NULL_HANDLE;
			VkFence inFlight = VK_NULL_HANDLE;
		};

		vkb::Swapchain m_swapchain{};
		std::vector<VkImage> m_images;
		std::vector<VkImageView> m_imageViews;
		VkImage m_depthImage = VK_NULL_HANDLE;
		VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
		VmaAllocator m_allocator = nullptr;
		VkImageView m_depthView = VK_NULL_HANDLE;
		gpu::Format m_depthFormat = gpu::Format::Undefined;
		// One renderFinished semaphore per swapchain image: by the time an image is
		// re-acquired, the presentation engine must have consumed its semaphore.
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
		FrameSync m_frames[kMaxFramesInFlight]{};
		std::uint32_t m_currentFrame = 0;
		std::uint32_t m_imageIndex = 0;
		std::uint32_t m_graphicsQueueFamily = 0;
		bool m_frameValid = false;
		bool m_needsRecreation = false;
		VkImageLayout m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkDevice m_device = VK_NULL_HANDLE;
		bool m_shutdown = false;
	};
} // namespace aether
