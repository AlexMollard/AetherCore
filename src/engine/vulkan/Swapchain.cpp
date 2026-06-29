#include "vulkan/Swapchain.hpp"

#include <chrono>
#include <format>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "utils/AetherExceptions.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "platform/Window.hpp"

namespace aether
{
	namespace
	{
		gpu::Format PickDepthFormat(const VkPhysicalDevice physicalDevice)
		{
			constexpr VkFormat kCandidates[] = {
			        VK_FORMAT_D32_SFLOAT,
			        VK_FORMAT_D16_UNORM,
			};

			for (const VkFormat format: kCandidates)
			{
				VkFormatProperties props{};
				vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
				if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
				{
					return gpu::FromVk(format);
				}
			}

			Throw(AetherError::Vulkan(0, "Failed to find a supported depth format."));
		}
	} // namespace

	void Swapchain::Initialize(const VulkanContext& ctx, const Window& window, const bool enableVsync)
	{
		int w = 0;
		int h = 0;
		glfwGetFramebufferSize(window.GetHandle(), &w, &h);

		auto buildSwapchain = [&](const VkPresentModeKHR presentMode)
		{
			vkb::SwapchainBuilder builder{ctx.GetDevice()};
			return builder.set_desired_format({.format = VK_FORMAT_B8G8R8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
			        .add_fallback_format({.format = VK_FORMAT_R8G8B8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
			        .set_desired_present_mode(presentMode)
			        .set_desired_extent(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h))
			        .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
			        .build();
		};

		auto result = buildSwapchain(enableVsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR);
		if (!result && !enableVsync)
		{
			AE_WARN(LogCategory::Engine, "Swapchain IMMEDIATE present mode unavailable; falling back to FIFO (VSync on).");
			result = buildSwapchain(VK_PRESENT_MODE_FIFO_KHR);
		}

		if (!result)
		{
			Throw(AetherError::Vulkan(0, "Failed to create swapchain: " + result.error().message()));
		}

		m_swapchain = result.value();
		m_images = m_swapchain.get_images().value();
		m_imageViews = m_swapchain.get_image_views().value();
		m_depthFormat = PickDepthFormat(ctx.GetDevice().physical_device);
		m_allocator = ctx.GetAllocator();
		m_graphicsQueueFamily = ctx.GetGraphicsQueueFamily();

		VkDevice device = ctx.GetDevice().device;
		m_device = device;
		m_shutdown = false;

		const VkImageCreateInfo depthImageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = gpu::ToVk(m_depthFormat),
		        .extent =
		                {
		                        .width = m_swapchain.extent.width,
		                        .height = m_swapchain.extent.height,
		                        .depth = 1,
		                },
		        .mipLevels = 1,
		        .arrayLayers = 1,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		const VmaAllocationCreateInfo depthAllocInfo{
		        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		};
		VmaAllocationInfo allocInfo{};
		const VkResult depthResult = vmaCreateImage(m_allocator, &depthImageInfo, &depthAllocInfo, &m_depthImage, &m_depthAllocation, &allocInfo);
		if (depthResult != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(static_cast<int32_t>(depthResult), "Failed to create depth image."));
		}

		const VkImageViewCreateInfo depthViewInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		        .image = m_depthImage,
		        .viewType = VK_IMAGE_VIEW_TYPE_2D,
		        .format = gpu::ToVk(m_depthFormat),
		        .subresourceRange =
		                {
		                        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
		                        .baseMipLevel = 0,
		                        .levelCount = 1,
		                        .baseArrayLayer = 0,
		                        .layerCount = 1,
		                },
		};
		if (vkCreateImageView(device, &depthViewInfo, nullptr, &m_depthView) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to create depth image view."));
		}

		for (auto& frame: m_frames)
		{
			VkCommandPoolCreateInfo poolInfo{};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

			if (vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "Failed to create swapchain command pool."));
			}

			VkCommandBufferAllocateInfo bufAllocInfo{};
			bufAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			bufAllocInfo.commandPool = frame.commandPool;
			bufAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			bufAllocInfo.commandBufferCount = 1;

			if (vkAllocateCommandBuffers(device, &bufAllocInfo, &frame.commandBuffer) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "Failed to allocate swapchain command buffer."));
			}

			const VkSemaphoreCreateInfo semInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			if (vkCreateSemaphore(device, &semInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "Failed to create image available semaphore."));
			}

			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			if (vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "Failed to create in-flight fence."));
			}
		}

		const VkSemaphoreCreateInfo semInfo2{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		m_renderFinishedSemaphores.resize(m_images.size(), VK_NULL_HANDLE);
		for (auto& sem: m_renderFinishedSemaphores)
		{
			if (vkCreateSemaphore(device, &semInfo2, nullptr, &sem) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "Failed to create render finished semaphore."));
			}
		}

		// Name swapchain images and per-frame command buffers for RenderDoc / validation.
		for (std::size_t i = 0; i < m_images.size(); ++i)
		{
			const std::string imgName = std::format("Swapchain.Color[{}]", i);
			const std::string viewName = std::format("Swapchain.Color[{}].View", i);
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_images[i]), VK_OBJECT_TYPE_IMAGE, imgName.c_str());
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_imageViews[i]), VK_OBJECT_TYPE_IMAGE_VIEW, viewName.c_str());
		}
		vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_depthImage), VK_OBJECT_TYPE_IMAGE, "Swapchain.Depth");
		vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_depthView), VK_OBJECT_TYPE_IMAGE_VIEW, "Swapchain.Depth.View");
		for (std::size_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			const std::string cbName = std::format("Swapchain.CmdBuf[{}]", i);
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_frames[i].commandBuffer), VK_OBJECT_TYPE_COMMAND_BUFFER, cbName.c_str());
		}

		AE_INFO(LogCategory::Vulkan, "Swapchain initialized. {}x{} format={}", m_swapchain.extent.width, m_swapchain.extent.height, static_cast<int>(m_swapchain.image_format));
	}

	void Swapchain::Shutdown(VkDevice device)
	{
		if (device == VK_NULL_HANDLE || m_shutdown)
		{
			return;
		}
		m_shutdown = true;

		if (m_depthView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, m_depthView, nullptr);
			m_depthView = VK_NULL_HANDLE;
		}
		if (m_depthImage != VK_NULL_HANDLE)
		{
			vmaDestroyImage(m_allocator, m_depthImage, m_depthAllocation);
			m_depthImage = VK_NULL_HANDLE;
			m_depthAllocation = VK_NULL_HANDLE;
		}
		m_depthFormat = gpu::Format::Undefined;

		for (auto& frame: m_frames)
		{
			if (frame.inFlight != VK_NULL_HANDLE)
			{
				vkDestroyFence(device, frame.inFlight, nullptr);
				frame.inFlight = VK_NULL_HANDLE;
			}
			if (frame.imageAvailable != VK_NULL_HANDLE)
			{
				vkDestroySemaphore(device, frame.imageAvailable, nullptr);
				frame.imageAvailable = VK_NULL_HANDLE;
			}
			if (frame.commandPool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(device, frame.commandPool, nullptr);
				frame.commandPool = VK_NULL_HANDLE;
				frame.commandBuffer = VK_NULL_HANDLE;
			}
		}

		for (auto& sem: m_renderFinishedSemaphores)
		{
			if (sem != VK_NULL_HANDLE)
			{
				vkDestroySemaphore(device, sem, nullptr);
			}
		}
		m_renderFinishedSemaphores.clear();

		for (auto view: m_imageViews)
		{
			vkDestroyImageView(device, view, nullptr);
		}
		m_imageViews.clear();
		m_images.clear();

		vkb::destroy_swapchain(m_swapchain);
	}

	void Swapchain::BeginFrame(VkDevice device)
	{
		m_frameValid = false;

		FrameSync& frame = m_frames[m_currentFrame];

		{
			const auto fenceStart = std::chrono::steady_clock::now();
			AE_PROFILE_ZONE();
			if (vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "Failed to wait for fence."));
			}
			AE_PROFILE_PLOT("Swapchain/FenceWaitNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - fenceStart).count()));
		}

		VkResult acquireResult;
		{
			const auto acquireStart = std::chrono::steady_clock::now();
			AE_PROFILE_ZONE();
			acquireResult = vkAcquireNextImageKHR(device, m_swapchain.swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);
			AE_PROFILE_PLOT("Swapchain/AcquireImageNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - acquireStart).count()));
		}

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			AE_WARN(LogCategory::Vulkan, "Swapchain out of date - recreation needed.");
			m_needsRecreation = true;
			return;
		}

		if (acquireResult == VK_SUBOPTIMAL_KHR)
		{
			// Suboptimal: we can still present this frame, but request recreation
			// afterwards.
			AE_WARN(LogCategory::Vulkan, "Swapchain suboptimal - will recreate after present.");
			m_needsRecreation = true;
		}

		if (vkResetFences(device, 1, &frame.inFlight) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to reset in-flight fence."));
		}
		if (vkResetCommandPool(device, frame.commandPool, 0) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to reset command pool."));
		}

		const VkCommandBufferBeginInfo beginInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to begin commands buffer."));
		}

		// Transition: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL.
		vkutil::TransitionImage(frame.commandBuffer,
		        m_images[m_imageIndex],
		        VK_IMAGE_LAYOUT_UNDEFINED,
		        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		        VK_ACCESS_2_NONE,
		        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

		// Depth layout transition. On the first frame the image is truly UNDEFINED;
		// on subsequent frames the prior render pass left it as DEPTH_ATTACHMENT_OPTIMAL.
		// Using the tracked layout avoids a spec violation (UNDEFINED with non-TOP_OF_PIPE
		// src stages) and satisfies sync validation's write-after-write hazard check
		// for the single shared depth image.
		const bool depthIsFirstFrame = (m_depthLayout == VK_IMAGE_LAYOUT_UNDEFINED);
		const VkPipelineStageFlags2 depthSrcStage = depthIsFirstFrame ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
		const VkAccessFlags2 depthSrcAccess = depthIsFirstFrame ? VK_ACCESS_2_NONE : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		vkutil::TransitionImage(frame.commandBuffer,
		        m_depthImage,
		        m_depthLayout,
		        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		        depthSrcStage,
		        depthSrcAccess,
		        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
		        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		        VK_IMAGE_ASPECT_DEPTH_BIT);
		m_depthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

		m_frameValid = true;
	}

	void Swapchain::SubmitAndPresent(VkQueue graphicsQueue, VkQueue presentQueue, gpu::TimelineSemaphoreHandle extraWaitSemaphore, std::uint64_t extraWaitValue)
	{
		// Resolve gpu::TimelineSemaphoreHandle pImpl -> VkSemaphore at the seam.
		auto vkExtraWait = extraWaitSemaphore ? extraWaitSemaphore->semaphore : VK_NULL_HANDLE;

		if (!m_frameValid)
		{
			m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
			return;
		}

		FrameSync& frame = m_frames[m_currentFrame];
		VkCommandBuffer cmd = frame.commandBuffer;

		vkutil::TransitionImage(cmd,
		        m_images[m_imageIndex],
		        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
		        VK_ACCESS_2_NONE);

		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to end command buffer."));
		}

		VkSemaphore renderFinished = m_renderFinishedSemaphores[m_imageIndex];

		VkSemaphoreSubmitInfo imageWait{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = frame.imageAvailable,
		        .value = 0,
		        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		};

		VkSemaphoreSubmitInfo extraWait{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = vkExtraWait,
		        .value = extraWaitValue,
		        .stageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
		};

		VkSemaphoreSubmitInfo waitInfos[2] = {imageWait, extraWait};
		std::uint32_t waitCount = 1;
		if (extraWaitSemaphore != VK_NULL_HANDLE)
		{
			waitCount = 2;
		}

		VkCommandBufferSubmitInfo cmdInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = cmd,
		};

		VkSemaphoreSubmitInfo signalInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = renderFinished,
		        .value = 0,
		        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		VkSubmitInfo2 submit{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .waitSemaphoreInfoCount = waitCount,
		        .pWaitSemaphoreInfos = waitInfos,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmdInfo,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &signalInfo,
		};
		{
			const VkResult submitResult = vkQueueSubmit2(graphicsQueue, 1, &submit, frame.inFlight);
			if (submitResult == VK_ERROR_DEVICE_LOST)
			{
				AE_ERROR(LogCategory::Vulkan, "VK_ERROR_DEVICE_LOST on vkQueueSubmit2 (frame {}). GPU has crashed - check validation output above.", m_currentFrame);
				std::terminate();
			}
			if (submitResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "Failed to submit queue."));
			}
		}

		const VkPresentInfoKHR presentInfo{
		        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		        .waitSemaphoreCount = 1,
		        .pWaitSemaphores = &renderFinished,
		        .swapchainCount = 1,
		        .pSwapchains = &m_swapchain.swapchain,
		        .pImageIndices = &m_imageIndex,
		};
		const VkResult presentResult = vkQueuePresentKHR(presentQueue, &presentInfo);
		if (presentResult == VK_ERROR_DEVICE_LOST)
		{
			AE_ERROR(LogCategory::Vulkan, "VK_ERROR_DEVICE_LOST on vkQueuePresentKHR (frame {}). GPU has crashed - check validation output above.", m_currentFrame);
			std::terminate();
		}
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
		{
			m_needsRecreation = true;
		}

		m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
	}

	VkCommandBuffer Swapchain::GetCurrentCommandBuffer() const
	{
		if (!m_frameValid)
		{
			return VK_NULL_HANDLE;
		}
		return m_frames[m_currentFrame].commandBuffer;
	}

	gpu::Extent2D Swapchain::GetExtent() const
	{
		return gpu::Extent2D(m_swapchain.extent);
	}

	gpu::Format Swapchain::GetImageFormat() const
	{
		return gpu::FromVk(m_swapchain.image_format);
	}

	gpu::Format Swapchain::GetDepthFormat() const
	{
		return m_depthFormat;
	}

	VkImage Swapchain::GetCurrentImage() const
	{
		return m_frameValid ? m_images[m_imageIndex] : VK_NULL_HANDLE;
	}

	VkImageView Swapchain::GetCurrentImageView() const
	{
		return m_frameValid ? m_imageViews[m_imageIndex] : VK_NULL_HANDLE;
	}

	std::uint32_t Swapchain::GetCurrentImageIndex() const
	{
		return m_frameValid ? m_imageIndex : UINT32_MAX;
	}

	VkImage Swapchain::GetDepthImage() const
	{
		return m_depthImage;
	}

	VkImageView Swapchain::GetDepthImageView() const
	{
		return m_depthView;
	}

	bool Swapchain::IsFrameValid() const
	{
		return m_frameValid;
	}

	bool Swapchain::NeedsRecreation() const
	{
		return m_needsRecreation;
	}

	void Swapchain::ClearRecreationFlag()
	{
		m_needsRecreation = false;
	}
} // namespace aether
