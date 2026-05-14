#include "gpu/AsyncComputeContext.hpp"

#include <stdexcept>
#include <string>

#include "gpu/GpuDevice.hpp"
#include "rendering/CommandRecorder.hpp"
#include "utils/Expected.hpp"
#include "vulkan/VulkanContext.hpp"

#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

namespace aether
{
	AsyncComputeContext::~AsyncComputeContext()
	{
	}

	void AsyncComputeContext::Init(GpuDevice& gpu)
	{
		VulkanContext& vk = gpu.GetVulkanContext();
		VkDevice device = vk.GetDevice().device;

		const VkSemaphoreTypeCreateInfo timelineTypeInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			.initialValue = 0,
		};
		const VkSemaphoreCreateInfo semInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &timelineTypeInfo,
		};
		VkSemaphore semaphore = VK_NULL_HANDLE;
		if (vkCreateSemaphore(device, &semInfo, nullptr, &semaphore) != VK_SUCCESS)
		{
			throw std::runtime_error("AsyncComputeContext: failed to create compute timeline semaphore.");
		}
		m_timelineSemaphoreHandle = reinterpret_cast<std::uint64_t>(semaphore);
		CommandRecorder::SetObjectName(device, m_timelineSemaphoreHandle, VK_OBJECT_TYPE_SEMAPHORE, "AsyncCompute.Timeline");

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (std::size_t frameI = 0; frameI < m_frames.size(); ++frameI)
		{
			auto& frame = m_frames[frameI];

			const VkCommandPoolCreateInfo poolInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
				.queueFamilyIndex = vk.GetComputeQueueFamily(),
			};
			VkCommandPool pool = VK_NULL_HANDLE;
			if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
			{
				throw std::runtime_error("AsyncComputeContext: failed to create async compute command pool.");
			}
			frame.commandPool = reinterpret_cast<std::uint64_t>(pool);

			const VkCommandBufferAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS)
			{
				throw std::runtime_error("AsyncComputeContext: failed to allocate async compute command buffer.");
			}
			frame.commandBuffer = reinterpret_cast<std::uint64_t>(cmd);

			VkFence fence = VK_NULL_HANDLE;
			if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				throw std::runtime_error("AsyncComputeContext: failed to create async compute fence.");
			}
			frame.fence = reinterpret_cast<std::uint64_t>(fence);

			const std::string suffix = "[" + std::to_string(frameI) + "]";
			CommandRecorder::SetObjectName(device, frame.commandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER, ("AsyncCompute.Cmd" + suffix).c_str());
			CommandRecorder::SetObjectName(device, frame.fence, VK_OBJECT_TYPE_FENCE, ("AsyncCompute.Fence" + suffix).c_str());
		}

		m_enabled = true;
		m_initialized = true;
	}

	void AsyncComputeContext::Shutdown(GpuDevice& gpu)
	{
		if (!m_initialized)
		{
			return;
		}

		VkDevice device = gpu.GetVulkanContext().GetDevice().device;

		for (auto& frame: m_frames)
		{
			if (frame.fence != 0)
			{
				vkDestroyFence(device, reinterpret_cast<VkFence>(frame.fence), nullptr);
				frame.fence = 0;
			}
			if (frame.commandPool != 0)
			{
				vkDestroyCommandPool(device, reinterpret_cast<VkCommandPool>(frame.commandPool), nullptr);
				frame.commandPool = 0;
				frame.commandBuffer = 0;
			}
		}

		if (m_timelineSemaphoreHandle != 0)
		{
			vkDestroySemaphore(device, reinterpret_cast<VkSemaphore>(m_timelineSemaphoreHandle), nullptr);
			m_timelineSemaphoreHandle = 0;
		}

		m_initialized = false;
		m_enabled = false;
	}

	void AsyncComputeContext::BeginFrame(GpuDevice& gpu, std::uint32_t frameIndex)
	{
		VkDevice device = gpu.GetVulkanContext().GetDevice().device;
		auto& frame = m_frames[frameIndex];
		VkFence fence = reinterpret_cast<VkFence>(frame.fence);
		VkCommandPool pool = reinterpret_cast<VkCommandPool>(frame.commandPool);

		if (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		{
			throw VulkanError("AsyncComputeContext: failed to wait for fence.");
		}
		if (vkResetFences(device, 1, &fence) != VK_SUCCESS)
		{
			throw VulkanError("AsyncComputeContext: failed to reset fence.");
		}
		if (vkResetCommandPool(device, pool, 0) != VK_SUCCESS)
		{
			throw VulkanError("AsyncComputeContext: failed to reset command pool.");
		}

		VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(frame.commandBuffer);
		const VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		{
			throw VulkanError("AsyncComputeContext: failed to begin command buffer.");
		}
		CommandRecorder(cmd).BeginDebugLabel("AsyncCompute.LightCull", 0.9f, 0.45f, 0.1f);
	}

	CommandRecorder AsyncComputeContext::GetCommandRecorder(std::uint32_t frameIndex) const
	{
		return CommandRecorder(reinterpret_cast<VkCommandBuffer>(m_frames[frameIndex].commandBuffer));
	}

	void AsyncComputeContext::EndCommandBuffer(std::uint32_t frameIndex)
	{
		VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(m_frames[frameIndex].commandBuffer);
		CommandRecorder(cmd).EndDebugLabel();
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		{
			throw VulkanError("AsyncComputeContext: failed to end command buffer.");
		}
	}

	AsyncComputeContext::SubmitResult AsyncComputeContext::Submit(GpuDevice& gpu, std::uint32_t frameIndex)
	{
		auto& frame = m_frames[frameIndex];
		const std::uint64_t signalValue = ++m_timelineValue;

		const VkTimelineSemaphoreSubmitInfo timelineSubmit{
			.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
			.waitSemaphoreValueCount = 0,
			.pWaitSemaphoreValues = nullptr,
			.signalSemaphoreValueCount = 1,
			.pSignalSemaphoreValues = &signalValue,
		};

		VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(frame.commandBuffer);
		VkFence fence = reinterpret_cast<VkFence>(frame.fence);
		VkSemaphore timelineSem = reinterpret_cast<VkSemaphore>(m_timelineSemaphoreHandle);

		const VkSubmitInfo submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = &timelineSubmit,
			.waitSemaphoreCount = 0,
			.pWaitSemaphores = nullptr,
			.pWaitDstStageMask = nullptr,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &timelineSem,
		};

		if (vkQueueSubmit(gpu.GetVulkanContext().GetComputeQueue(), 1, &submitInfo, fence) != VK_SUCCESS)
		{
			throw VulkanError("AsyncComputeContext: failed to submit queue.");
		}

		return {
			.semaphoreHandle = m_timelineSemaphoreHandle,
			.timelineValue = signalValue,
		};
	}
} // namespace aether
