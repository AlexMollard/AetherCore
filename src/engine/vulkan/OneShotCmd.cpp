#include "gpu/OneShotCmd.hpp"

#include "vulkan/volk.hpp"

#include "utils/Assert.hpp"

namespace aether::gpu
{
	OneShotCmd::~OneShotCmd()
	{
		Release();
	}

	OneShotCmd::OneShotCmd(OneShotCmd&& other) noexcept
	      : m_device(std::exchange(other.m_device, nullptr)), m_pool(std::exchange(other.m_pool, nullptr)), m_cmd(std::exchange(other.m_cmd, nullptr)), m_cmdList(std::move(other.m_cmdList))
	{
	}

	OneShotCmd& OneShotCmd::operator=(OneShotCmd&& other) noexcept
	{
		if (this != &other)
		{
			Release();
			m_device = std::exchange(other.m_device, nullptr);
			m_pool = std::exchange(other.m_pool, nullptr);
			m_cmd = std::exchange(other.m_cmd, nullptr);
			m_cmdList = std::move(other.m_cmdList);
		}
		return *this;
	}

	bool OneShotCmd::Begin(void* device, void* pool)
	{
		if (m_cmd != nullptr)
		{
			Release();
		}

		const VkDevice vkDevice = static_cast<VkDevice>(device);
		const VkCommandPool vkPool = static_cast<VkCommandPool>(pool);

		const VkCommandBufferAllocateInfo ai{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool = vkPool,
		        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(vkDevice, &ai, &cmd) != VK_SUCCESS)
		{
			return false;
		}

		const VkCommandBufferBeginInfo bi{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
		{
			vkFreeCommandBuffers(vkDevice, vkPool, 1, &cmd);
			return false;
		}

		m_device = device;
		m_pool = pool;
		m_cmd = static_cast<void*>(cmd);
		m_cmdList = gpu::CommandList(static_cast<void*>(cmd));
		return true;
	}

	gpu::CommandList& OneShotCmd::CmdList()
	{
		return m_cmdList;
	}

	bool OneShotCmd::EndAndSubmit(void* queue)
	{
		if (m_cmd == nullptr)
		{
			return false;
		}

		const VkDevice vkDevice = static_cast<VkDevice>(m_device);
		const VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(m_cmd);
		const VkQueue vkQueue = static_cast<VkQueue>(queue);

		VkResult result = vkEndCommandBuffer(vkCmd);
		if (result != VK_SUCCESS)
		{
			Release();
			return false;
		}

		const VkCommandBufferSubmitInfo cbInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = vkCmd,
		};
		const VkSubmitInfo2 si{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cbInfo,
		};

		const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence fence = VK_NULL_HANDLE;
		if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
		{
			Release();
			return false;
		}

		result = vkQueueSubmit2(vkQueue, 1, &si, fence);
		if (result != VK_SUCCESS)
		{
			vkDestroyFence(vkDevice, fence, nullptr);
			Release();
			return false;
		}

		(void) vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
		vkDestroyFence(vkDevice, fence, nullptr);

		Release();
		return true;
	}

	void OneShotCmd::Release()
	{
		if (m_cmd != nullptr)
		{
			const VkDevice vkDevice = static_cast<VkDevice>(m_device);
			const VkCommandPool vkPool = static_cast<VkCommandPool>(m_pool);
			const VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(m_cmd);
			vkFreeCommandBuffers(vkDevice, vkPool, 1, &vkCmd);
		}
		m_device = nullptr;
		m_pool = nullptr;
		m_cmd = nullptr;
		m_cmdList = gpu::CommandList(nullptr);
	}
} // namespace aether::gpu
