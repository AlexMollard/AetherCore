#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"
#include "vulkan/volk.hpp"

namespace aether::gpu
{
	namespace detail
	{
		struct TimelineSemaphoreData
		{
			VkSemaphore semaphore = VK_NULL_HANDLE;
		};
	} // namespace detail

	using TimelineSemaphore = detail::TimelineSemaphoreData;
	using TimelineSemaphoreHandle = TimelineSemaphore*;

	struct TimelineSemaphoreDesc
	{
		Device device = nullptr;
		std::uint64_t initialValue = 0;
		const char* debugName = nullptr;
	};

	[[nodiscard]] TimelineSemaphoreHandle CreateTimelineSemaphore(const TimelineSemaphoreDesc& desc) noexcept;
	[[nodiscard]] bool WaitTimelineSemaphore(Device device, TimelineSemaphoreHandle sem, std::uint64_t value) noexcept;
	void DestroyTimelineSemaphore(Device device, TimelineSemaphoreHandle sem) noexcept;

} // namespace aether::gpu
