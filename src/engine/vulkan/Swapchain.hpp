#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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
		[[nodiscard]] std::uint32_t GetCurrentImageIndex() const;
		[[nodiscard]] VkImage GetDepthImage() const;
		[[nodiscard]] VkImageView GetDepthImageView() const;
		[[nodiscard]] bool IsFrameValid() const;
		[[nodiscard]] bool NeedsRecreation() const;
		void ClearRecreationFlag();
		// Forces recreation on the next producer-thread poll (e.g. a VSync change
		// that alters the present mode without a resize).
		void RequestRecreation();

		// Invoked in SubmitAndPresent while the acquired image is still in
		// COLOR_ATTACHMENT (before the present transition), so a consumer (the
		// screenshot service) can copy the composited frame out of an owned,
		// correctly-laid-out image instead of racing a presented one. Args are the
		// frame command buffer (VkCommandBuffer) and the current image (VkImage) as
		// opaque pointers, plus the swapchain extent — keeps callers Vulkan-free.
		void SetPrePresentCapture(std::function<void(void* cmd, void* image, gpu::Extent2D extent)> callback)
		{
			m_prePresentCapture = std::move(callback);
		}

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
		// Written on the render thread (acquire/present OUT_OF_DATE/SUBOPTIMAL) and
		// polled on the main thread by the quiesced recreate; atomic to make that
		// cross-thread read/write race-free.
		std::atomic<bool> m_needsRecreation{false};
		VkImageLayout m_depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkDevice m_device = VK_NULL_HANDLE;
		bool m_shutdown = false;
		std::function<void(void* cmd, void* image, gpu::Extent2D extent)> m_prePresentCapture;
	};
} // namespace aether
