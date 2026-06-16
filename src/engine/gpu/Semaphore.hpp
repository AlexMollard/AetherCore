#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// Timeline-semaphore handle. pImpl defined in vulkan/Semaphore.cpp.
	namespace detail
	{
		struct TimelineSemaphoreData;
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

	// Cast a handle to its underlying backend semaphore. `void*` so the
	// engine-side header never names the backend type.
	[[nodiscard]] void* ResolveTimelineSemaphoreVk(TimelineSemaphoreHandle sem) noexcept;

	// Wrap an externally-created backend semaphore in a pImpl handle so
	// the engine can hold it as a typed TimelineSemaphoreHandle. The
	// returned handle owns the destroy path.
	[[nodiscard]] TimelineSemaphoreHandle WrapTimelineSemaphoreVk(void* vkSemaphore) noexcept;
} // namespace aether::gpu
