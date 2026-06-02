#include "gpu/AsyncComputeContext.hpp"

#include <stdexcept>
#include <string>

#include "gpu/GpuDevice.hpp"
#include "utils/Profiler.hpp"
#include "rendering/CommandRecorder.hpp"
#include "utils/Expected.hpp"
#include "vulkan/VulkanContext.hpp"

#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

namespace aether
{
	AsyncComputeContext::~AsyncComputeContext()
	{
		if (m_initialized && m_gpu)
		{
			Shutdown(*m_gpu);
		}
	}

	void AsyncComputeContext::Init(GpuDevice& gpu)
	{
		AE_PROFILE_ZONE();
		VulkanContext& vk = gpu.GetVulkanContext();
		VkDevice device = vk.GetDevice().device;

		m_gpu = &gpu;

		struct InitCleanup
		{
			VkDevice device = VK_NULL_HANDLE;
			VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
			std::vector<VkCommandPool> commandPools;
			std::vector<VkFence> fences;

			~InitCleanup()
			{
				if (device == VK_NULL_HANDLE)
				{
					return;
				}
				for (auto fence: fences)
				{
					if (fence != VK_NULL_HANDLE)
					{
						vkDestroyFence(device, fence, nullptr);
					}
				}
				for (auto pool: commandPools)
				{
					if (pool != VK_NULL_HANDLE)
					{
						vkDestroyCommandPool(device, pool, nullptr);
					}
				}
				if (timelineSemaphore != VK_NULL_HANDLE)
				{
					vkDestroySemaphore(device, timelineSemaphore, nullptr);
				}
			}

			void Disarm()
			{
				device = VK_NULL_HANDLE;
			}
		} cleanup;

		cleanup.device = device;

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
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to create compute timeline semaphore."));
		}
		m_timelineSemaphoreHandle = reinterpret_cast<std::uint64_t>(semaphore);
		cleanup.timelineSemaphore = semaphore;
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
				Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to create async compute command pool."));
			}
			frame.commandPool = reinterpret_cast<std::uint64_t>(pool);
			cleanup.commandPools.push_back(pool);

			const VkCommandBufferAllocateInfo allocInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			        .commandPool = pool,
			        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			        .commandBufferCount = 1,
			};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to allocate async compute command buffer."));
			}
			frame.commandBuffer = reinterpret_cast<std::uint64_t>(cmd);

			VkFence fence = VK_NULL_HANDLE;
			if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to create async compute fence."));
			}
			frame.fence = reinterpret_cast<std::uint64_t>(fence);
			cleanup.fences.push_back(fence);

			const std::string suffix = "[" + std::to_string(frameI) + "]";
			CommandRecorder::SetObjectName(device, frame.commandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER, ("AsyncCompute.Cmd" + suffix).c_str());
			CommandRecorder::SetObjectName(device, frame.fence, VK_OBJECT_TYPE_FENCE, ("AsyncCompute.Fence" + suffix).c_str());
		}

		cleanup.Disarm();
		m_enabled = true;
		m_initialized = true;
	}

	void AsyncComputeContext::Shutdown(GpuDevice& gpu)
	{
		AE_PROFILE_ZONE();
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
		AE_PROFILE_ZONE();
		VkDevice device = gpu.GetVulkanContext().GetDevice().device;
		auto& frame = m_frames[frameIndex];
		VkFence fence = reinterpret_cast<VkFence>(frame.fence);
		VkCommandPool pool = reinterpret_cast<VkCommandPool>(frame.commandPool);

		if (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to wait for fence."));
		}
		if (vkResetFences(device, 1, &fence) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to reset fence."));
		}
		if (vkResetCommandPool(device, pool, 0) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to reset command pool."));
		}

		VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(frame.commandBuffer);
		const VkCommandBufferBeginInfo beginInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to begin command buffer."));
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
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to end command buffer."));
		}
	}

	AsyncComputeContext::SubmitResult AsyncComputeContext::Submit(GpuDevice& gpu, std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		auto& frame = m_frames[frameIndex];
		const std::uint64_t signalValue = ++m_timelineValue;

		VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(frame.commandBuffer);
		VkFence fence = reinterpret_cast<VkFence>(frame.fence);
		VkSemaphore timelineSem = reinterpret_cast<VkSemaphore>(m_timelineSemaphoreHandle);

		VkCommandBufferSubmitInfo cmdInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = cmd,
		};

		VkSemaphoreSubmitInfo signalInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = timelineSem,
		        .value = signalValue,
		        .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		};

		VkSubmitInfo2 submitInfo{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmdInfo,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &signalInfo,
		};

		if (vkQueueSubmit2(gpu.GetVulkanContext().GetComputeQueue(), 1, &submitInfo, fence) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AsyncComputeContext: failed to submit queue."));
		}

		return {
		        .semaphoreHandle = m_timelineSemaphoreHandle,
		        .timelineValue = signalValue,
		};
	}
} // namespace aether
