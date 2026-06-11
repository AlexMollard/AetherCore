#include "gpu/Semaphore.hpp"

#include "vulkan/volk.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/LogCategory.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::gpu
{
	TimelineSemaphore* CreateTimelineSemaphore(const TimelineSemaphoreDesc& desc) noexcept
	{
		VkDevice device = static_cast<VkDevice>(desc.device);
		if (device == VK_NULL_HANDLE)
		{
			return nullptr;
		}

		const VkSemaphoreTypeCreateInfo timelineTypeInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		        .initialValue = desc.initialValue,
		};
		const VkSemaphoreCreateInfo semInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		        .pNext = &timelineTypeInfo,
		};

		VkSemaphore semaphore = VK_NULL_HANDLE;
		if (vkCreateSemaphore(device, &semInfo, nullptr, &semaphore) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "gpu::CreateTimelineSemaphore: vkCreateSemaphore failed.");
			return nullptr;
		}

		if (desc.debugName != nullptr)
		{
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(semaphore), VK_OBJECT_TYPE_SEMAPHORE, desc.debugName);
		}

		return reinterpret_cast<TimelineSemaphore*>(semaphore);
	}

	bool WaitTimelineSemaphore(void* device, TimelineSemaphore* sem, std::uint64_t value) noexcept
	{
		VkDevice vkDevice = static_cast<VkDevice>(device);
		VkSemaphore vkSem = reinterpret_cast<VkSemaphore>(sem);
		if (vkDevice == VK_NULL_HANDLE || vkSem == VK_NULL_HANDLE)
		{
			return false;
		}

		const VkSemaphoreWaitInfo waitInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		        .flags = 0,
		        .semaphoreCount = 1,
		        .pSemaphores = &vkSem,
		        .pValues = &value,
		};

		const VkResult res = vkWaitSemaphores(vkDevice, &waitInfo, UINT64_MAX);
		if (res != VK_SUCCESS && res != VK_TIMEOUT)
		{
			AE_WARN(LogCategory::Vulkan, "gpu::WaitTimelineSemaphore: vkWaitSemaphores returned {} (expected success or timeout).", static_cast<int>(res));
			return false;
		}
		return true;
	}

	void DestroyTimelineSemaphore(void* device, TimelineSemaphore* sem) noexcept
	{
		if (sem == nullptr)
		{
			return;
		}
		VkDevice vkDevice = static_cast<VkDevice>(device);
		VkSemaphore vkSem = reinterpret_cast<VkSemaphore>(sem);
		if (vkDevice != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(vkDevice, vkSem, nullptr);
		}
	}
} // namespace aether::gpu
