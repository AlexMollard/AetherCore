#include "vulkan/TransferManager.hpp"

#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/QueueSubmit.hpp"

namespace aether::vulkan
{
	TransferManager::~TransferManager()
	{
		Shutdown();
	}

	void TransferManager::Initialize(VkDevice device, VkQueue transferQueue, std::uint32_t transferFamily, std::uint32_t graphicsFamily)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(m_device == VK_NULL_HANDLE, "TransferManager: already initialized");

		const VkSemaphoreTypeCreateInfo timelineType{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		        .initialValue = 0,
		};
		const VkSemaphoreCreateInfo semInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		        .pNext = &timelineType,
		};
		if (vkCreateSemaphore(device, &semInfo, nullptr, &m_timeline) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "TransferManager: failed to create timeline semaphore"));
		}

		const VkCommandPoolCreateInfo poolInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = transferFamily,
		};
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS)
		{
			vkDestroySemaphore(device, m_timeline, nullptr);
			m_timeline = VK_NULL_HANDLE;
			Throw(AetherError::Vulkan(0, "TransferManager: failed to create command pool"));
		}

		m_device = device;
		m_queue = transferQueue;
		m_transferFamily = transferFamily;
		m_graphicsFamily = graphicsFamily;
		m_nextValue = 1;
		m_lastSubmitted.store(0, std::memory_order_release);

		AE_INFO(LogCategory::Vulkan, "TransferManager initialised on queue family {} ({}).", transferFamily, HasDedicatedFamily() ? "dedicated transfer" : "graphics fallback");
	}

	void TransferManager::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		AE_PROFILE_ZONE();

		WaitFor(m_lastSubmitted.load(std::memory_order_acquire));

		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			CollectCompleted();
			AE_ASSERT(m_inFlight.empty(), "TransferManager: in-flight uploads survived an idle wait");
		}

		vkDestroyCommandPool(m_device, m_pool, nullptr);
		vkDestroySemaphore(m_device, m_timeline, nullptr);
		m_pool = VK_NULL_HANDLE;
		m_timeline = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
		m_queue = VK_NULL_HANDLE;
	}

	TransferManager::Ticket TransferManager::Submit(const std::function<void(gpu::CommandList&)>& record, std::function<void()> onComplete)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(m_device != VK_NULL_HANDLE, "TransferManager: Submit before Initialize");

		const std::lock_guard<std::mutex> lock(m_mutex);
		CollectCompleted();

		const VkCommandBufferAllocateInfo allocInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .commandPool = m_pool,
		        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(m_device, &allocInfo, &cmd) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "TransferManager: failed to allocate command buffer"));
		}

		const VkCommandBufferBeginInfo beginInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		{
			vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
			Throw(AetherError::Vulkan(0, "TransferManager: failed to begin command buffer"));
		}

		gpu::CommandList list(static_cast<void*>(cmd));
		record(list);

		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		{
			vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
			Throw(AetherError::Vulkan(0, "TransferManager: failed to end command buffer"));
		}

		const Ticket value = m_nextValue;

		const VkCommandBufferSubmitInfo cbInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = cmd,
		};
		const VkSemaphoreSubmitInfo signalInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = m_timeline,
		        .value = value,
		        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};
		const VkSubmitInfo2 submit{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cbInfo,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &signalInfo,
		};

		VkResult submitResult = VK_SUCCESS;
		{
			// The global lock keeps the graphics-family fallback (transfer queue ==
			// graphics queue) externally synchronised against the render loop.
			const std::lock_guard<std::mutex> queueLock(QueueSubmitMutex());
			submitResult = vkQueueSubmit2(m_queue, 1, &submit, VK_NULL_HANDLE);
		}
		if (submitResult != VK_SUCCESS)
		{
			vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
			Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "TransferManager: failed to submit upload"));
		}

		m_inFlight.push_back(InFlight{.value = value, .cmd = cmd, .onComplete = std::move(onComplete)});
		++m_nextValue;
		m_lastSubmitted.store(value, std::memory_order_release);
		return value;
	}

	bool TransferManager::IsComplete(Ticket ticket) const
	{
		if (ticket == 0 || m_device == VK_NULL_HANDLE)
		{
			return true;
		}
		std::uint64_t counter = 0;
		if (vkGetSemaphoreCounterValue(m_device, m_timeline, &counter) != VK_SUCCESS)
		{
			return false;
		}
		return counter >= ticket;
	}

	void TransferManager::WaitFor(Ticket ticket)
	{
		if (ticket == 0 || m_device == VK_NULL_HANDLE || IsComplete(ticket))
		{
			return;
		}
		AE_PROFILE_ZONE_N("TransferManager.WaitFor");

		const VkSemaphoreWaitInfo waitInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		        .semaphoreCount = 1,
		        .pSemaphores = &m_timeline,
		        .pValues = &ticket,
		};
		AE_ASSERT_ALWAYS(vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX) == VK_SUCCESS, "TransferManager: timeline wait failed (device lost?)");

		const std::lock_guard<std::mutex> lock(m_mutex);
		CollectCompleted();
	}

	void TransferManager::CollectCompleted()
	{
		if (m_inFlight.empty())
		{
			return;
		}
		std::uint64_t counter = 0;
		if (vkGetSemaphoreCounterValue(m_device, m_timeline, &counter) != VK_SUCCESS)
		{
			return;
		}
		std::erase_if(m_inFlight, [&](InFlight& entry)
		{
			if (entry.value > counter)
			{
				return false;
			}
			vkFreeCommandBuffers(m_device, m_pool, 1, &entry.cmd);
			if (entry.onComplete)
			{
				entry.onComplete();
			}
			return true;
		});
	}
} // namespace aether::vulkan
