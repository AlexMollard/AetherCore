#pragma once

#include <cstdint>
#include <vector>
#include <VkBootstrap.h>
#include <vulkan/vulkan.h>

#include "UniqueImage.hpp"

namespace aether
{
	class VulkanContext;
	class Window;

	class Swapchain
	{
	public:
		static constexpr std::uint32_t kMaxFramesInFlight = 3;

		void Initialize(const VulkanContext& ctx, const Window& window);
		void Shutdown(VkDevice device);

		// Acquires next image and begins the command buffer + dynamic rendering.
		// Sets IsFrameValid() to false if the swapchain is out of date (skip
		// EndFrame).
		void BeginFrame(VkDevice device);

		// Ends dynamic rendering, submits, and presents. No-op if !IsFrameValid().
		void EndFrame(VkQueue graphicsQueue, VkQueue presentQueue, VkSemaphore extraWaitSemaphore = VK_NULL_HANDLE, VkPipelineStageFlags extraWaitStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, std::uint64_t extraWaitValue = 0);

		[[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const;
		[[nodiscard]] VkExtent2D GetExtent() const;
		[[nodiscard]] VkFormat GetImageFormat() const;
		[[nodiscard]] VkFormat GetDepthFormat() const;
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
		UniqueImage m_depthImage;
		VkImageView m_depthView = VK_NULL_HANDLE;
		VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
		// One renderFinished semaphore per swapchain image: by the time an image is
		// re-acquired, the presentation engine must have consumed its semaphore.
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
		FrameSync m_frames[kMaxFramesInFlight]{};
		std::uint32_t m_currentFrame = 0;
		std::uint32_t m_imageIndex = 0;
		std::uint32_t m_graphicsQueueFamily = 0;
		bool m_frameValid = false;
		bool m_needsRecreation = false;
	};
} // namespace aether
