#include "gpu/Semaphore.hpp"

#include "vulkan/volk.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/LogCategory.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::gpu::detail
{
	// pImpl data: the real definition is here, where `VkSemaphore` is
	// visible. The engine side only sees a pointer to this struct, so
	// the engine never touches a `VkSemaphore` or a `void*` payload.
	struct TimelineSemaphoreData
	{
		VkSemaphore semaphore = VK_NULL_HANDLE;
	};
} // namespace aether::gpu::detail

namespace aether::gpu
{
	TimelineSemaphoreHandle CreateTimelineSemaphore(const TimelineSemaphoreDesc& desc) noexcept
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

		auto* data = new detail::TimelineSemaphoreData;
		if (vkCreateSemaphore(device, &semInfo, nullptr, &data->semaphore) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "gpu::CreateTimelineSemaphore: vkCreateSemaphore failed.");
			delete data;
			return nullptr;
		}

		if (desc.debugName != nullptr)
		{
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(data->semaphore), VK_OBJECT_TYPE_SEMAPHORE, desc.debugName);
		}

		return data;
	}

	bool WaitTimelineSemaphore(Device device, TimelineSemaphoreHandle sem, std::uint64_t value) noexcept
	{
		VkDevice vkDevice = static_cast<VkDevice>(device);
		if (sem == nullptr)
		{
			return false;
		}
		VkSemaphore vkSem = sem->semaphore;
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

	void DestroyTimelineSemaphore(Device device, TimelineSemaphoreHandle sem) noexcept
	{
		if (sem == nullptr)
		{
			return;
		}
		VkDevice vkDevice = static_cast<VkDevice>(device);
		if (vkDevice != VK_NULL_HANDLE && sem->semaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(vkDevice, sem->semaphore, nullptr);
		}
		delete sem;
	}

	void* ResolveTimelineSemaphoreVk(TimelineSemaphoreHandle sem) noexcept
	{
		if (sem == nullptr)
		{
			return nullptr;
		}
		return sem->semaphore;
	}

	TimelineSemaphoreHandle WrapTimelineSemaphoreVk(void* vkSemaphore) noexcept
	{
		if (vkSemaphore == nullptr)
		{
			return nullptr;
		}
		auto* data = new detail::TimelineSemaphoreData;
		data->semaphore = static_cast<VkSemaphore>(vkSemaphore);
		return data;
	}
} // namespace aether::gpu
