#include "assets/AssetSubsystem.hpp"

#include <stdexcept>

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "scene/World.hpp"
#include "vulkan/ResourcePool.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void AssetSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		services.Register<PrimitiveMeshes>(m_primitiveMeshes);

		m_context = &services.Get<VulkanContext>();
		VulkanContext& vk = *m_context;
		BindlessManager& bindless = services.Get<BindlessManager>();
		World& world = services.Get<World>();

		// Upload command pool - transient, per-buffer reset.
		const VkCommandPoolCreateInfo uploadPoolInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = vk.GetGraphicsQueueFamily(),
		};
		if (vkCreateCommandPool(vk.GetDevice().device, &uploadPoolInfo, nullptr, &m_uploadPool) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to create upload command pool."));
		}

		m_materialBuffer.Initialize(vk);
		m_meshArena.Initialize(vk, {});
		m_meshUploadQueue.Initialize(vk);
		m_primitiveMeshes.Initialize(vk.GetDevice().device, vk.GetAllocator(), vk.GetGraphicsQueue(), m_uploadPool);
		m_assetManager.Initialize(vk, bindless, m_materialBuffer, world, m_uploadPool);
	}

	void AssetSubsystem::LinkRenderingDeps(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		m_assetManager.SetRenderQueue(services.Get<RenderQueue>());
		m_assetManager.SetShadowService(services.Get<ShadowService>());
		m_assetManager.SetRenderTargetService(services.Get<RenderTargetService>());
	}

	void AssetSubsystem::FlushMeshUploads()
	{
		AE_PROFILE_ZONE();
		if (!m_meshUploadQueue.HasPendingUploads())
		{
			return;
		}

		VulkanContext& vk = *m_context;
		const VkCommandBufferAllocateInfo allocInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool = m_uploadPool,
		        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(vk.GetDevice().device, &allocInfo, &cmd) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to allocate command buffer."));
		}

		const VkCommandBufferBeginInfo beginInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to begin command buffer."));
		}
		m_meshUploadQueue.Flush(cmd);
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to end command buffer."));
		}

		const VkCommandBufferSubmitInfo cbInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = cmd,
		};
		const VkSubmitInfo2 submitInfo{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cbInfo,
		};
		const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence fence = VK_NULL_HANDLE;
		if (vkCreateFence(vk.GetDevice().device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to create upload fence."));
		}
		if (vkQueueSubmit2(vk.GetGraphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS)
		{
			vkDestroyFence(vk.GetDevice().device, fence, nullptr);
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to submit queue."));
		}
		(void)vkWaitForFences(vk.GetDevice().device, 1, &fence, VK_TRUE, UINT64_MAX);
		vkDestroyFence(vk.GetDevice().device, fence, nullptr);

		vkFreeCommandBuffers(vk.GetDevice().device, m_uploadPool, 1, &cmd);
	}

	void AssetSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_assetManager = AssetManager{};
		m_primitiveMeshes.Destroy();
		m_meshUploadQueue.Shutdown();
		m_meshArena.Shutdown();
		m_materialBuffer.Shutdown();

		if (m_uploadPool != VK_NULL_HANDLE && m_context != nullptr)
		{
			vkDestroyCommandPool(m_context->GetDevice().device, m_uploadPool, nullptr);
			m_uploadPool = VK_NULL_HANDLE;
		}
	}
} // namespace aether
