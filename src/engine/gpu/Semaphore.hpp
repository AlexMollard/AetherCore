#pragma once

#include <cstdint>

namespace aether::gpu
{
	// Opaque timeline semaphore. Backend owns the VkSemaphore; engine code
	// treats this as void* (or a wrapped handle) and never sees vk* types.
	using TimelineSemaphore = void;

	// Create a binary-or-timeline semaphore. For timeline semaphores pass
	// initialValue > 0 via the desc. The device is a void* (VkDevice) that
	// the backend casts. Returns nullptr on failure.
	struct TimelineSemaphoreDesc
	{
		void*         device      = nullptr;
		std::uint64_t initialValue = 0;
		const char*   debugName    = nullptr;
	};

	[[nodiscard]] TimelineSemaphore* CreateTimelineSemaphore(const TimelineSemaphoreDesc& desc) noexcept;

	// Wait for a timeline semaphore to reach the given value. The device is
	// the same one passed to CreateTimelineSemaphore. Returns true on
	// success or timeout, false on error.
	[[nodiscard]] bool WaitTimelineSemaphore(void* device, TimelineSemaphore* sem, std::uint64_t value) noexcept;

	// Destroy a timeline semaphore. The device is the same one passed to
	// CreateTimelineSemaphore. The handle is invalid after this call.
	void DestroyTimelineSemaphore(void* device, TimelineSemaphore* sem) noexcept;
} // namespace aether::gpu
