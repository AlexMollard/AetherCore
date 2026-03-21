#include "Swapchain.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Logger.hpp"
#include "MeowExceptions.hpp"
#include "VulkanContext.hpp"
#include "VulkanUtils.hpp"
#include "Window.hpp"

namespace meow
{
	void Swapchain::Initialize(const VulkanContext& ctx, const Window& window)
	{
		int w = 0;
		int h = 0;
		glfwGetFramebufferSize(window.GetHandle(), &w, &h);

		vkb::SwapchainBuilder builder{ ctx.GetDevice() };
		auto result = builder
			.set_desired_format({ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
			.add_fallback_format({ VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
			.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
			.set_desired_extent(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h))
			.set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
			.build();

		if (!result)
		{
			throw VulkanError("Failed to create swapchain: " + result.error().message());
		}

		m_swapchain = result.value();
		m_images = m_swapchain.get_images().value();
		m_imageViews = m_swapchain.get_image_views().value();
		m_graphicsQueueFamily = ctx.GetGraphicsQueueFamily();

		VkDevice device = ctx.GetDevice().device;

		for (auto& frame : m_frames)
		{
			VkCommandPoolCreateInfo poolInfo{};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

			if (vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS)
			{
				throw VulkanError("Failed to create swapchain command pool.");
			}

			VkCommandBufferAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			allocInfo.commandPool = frame.commandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;

			if (vkAllocateCommandBuffers(device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS)
			{
				throw VulkanError("Failed to allocate swapchain command buffer.");
			}

			const VkSemaphoreCreateInfo semInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			vkCreateSemaphore(device, &semInfo, nullptr, &frame.imageAvailable);

			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight);
		}

		// One renderFinished semaphore per swapchain image.
		const VkSemaphoreCreateInfo semInfo2{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		m_renderFinishedSemaphores.resize(m_images.size(), VK_NULL_HANDLE);
		for (auto& sem : m_renderFinishedSemaphores)
		{
			vkCreateSemaphore(device, &semInfo2, nullptr, &sem);
		}

		INFO(LogCategory::Vulkan, "Swapchain initialized. {}x{} format={}",
			m_swapchain.extent.width, m_swapchain.extent.height,
			static_cast<int>(m_swapchain.image_format));
	}

	void Swapchain::Shutdown(VkDevice device)
	{
		if (device == VK_NULL_HANDLE)
		{
			return;
		}

		for (auto& frame : m_frames)
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

		for (auto& sem : m_renderFinishedSemaphores)
		{
			if (sem != VK_NULL_HANDLE)
			{
				vkDestroySemaphore(device, sem, nullptr);
			}
		}
		m_renderFinishedSemaphores.clear();

		for (auto view : m_imageViews)
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

		vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

		const VkResult acquireResult = vkAcquireNextImageKHR(
			device, m_swapchain.swapchain, UINT64_MAX,
			frame.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			WARN(LogCategory::Vulkan, "Swapchain out of date — skipping frame.");
			return;
		}

		vkResetFences(device, 1, &frame.inFlight);
		vkResetCommandPool(device, frame.commandPool, 0);

		const VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

		// Transition: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
		vkutil::TransitionImage(
			frame.commandBuffer, m_images[m_imageIndex],
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

		// Begin dynamic rendering
		const VkRenderingAttachmentInfo colorAttach{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = m_imageViews[m_imageIndex],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = {.color = { { 0.05f, 0.05f, 0.07f, 1.0f } } },
		};
		const VkRenderingInfo renderInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = { { 0, 0 }, m_swapchain.extent },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &colorAttach,
		};
		vkCmdBeginRendering(frame.commandBuffer, &renderInfo);

		// Set dynamic viewport and scissor
		const VkViewport viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = static_cast<float>(m_swapchain.extent.width),
			.height = static_cast<float>(m_swapchain.extent.height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		const VkRect2D scissor{ { 0, 0 }, m_swapchain.extent };
		vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
		vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

		m_frameValid = true;
	}

	void Swapchain::EndFrame(VkQueue graphicsQueue, VkQueue presentQueue)
	{
		if (!m_frameValid)
		{
			m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
			return;
		}

		FrameSync& frame = m_frames[m_currentFrame];
		VkCommandBuffer cmd = frame.commandBuffer;

		vkCmdEndRendering(cmd);

		// Transition: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
		vkutil::TransitionImage(
			cmd, m_images[m_imageIndex],
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);

		vkEndCommandBuffer(cmd);

		VkSemaphore renderFinished = m_renderFinishedSemaphores[m_imageIndex];

		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		const VkSubmitInfo submit{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &frame.imageAvailable,
			.pWaitDstStageMask = &waitStage,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &renderFinished,
		};
		vkQueueSubmit(graphicsQueue, 1, &submit, frame.inFlight);

		const VkPresentInfoKHR presentInfo{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &renderFinished,
			.swapchainCount = 1,
			.pSwapchains = &m_swapchain.swapchain,
			.pImageIndices = &m_imageIndex,
		};
		vkQueuePresentKHR(presentQueue, &presentInfo);

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

	VkExtent2D Swapchain::GetExtent() const
	{
		return m_swapchain.extent;
	}

	VkFormat Swapchain::GetImageFormat() const
	{
		return m_swapchain.image_format;
	}

	bool Swapchain::IsFrameValid() const
	{
		return m_frameValid;
	}
}
