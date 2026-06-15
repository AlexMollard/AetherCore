#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// Engine-side handle for a timeline semaphore. The full definition
	// lives in the vulkan backend (`vulkan/Semaphore.cpp`); the engine
	// only ever sees this forward-declared pImpl pointer, never a
	// `VkSemaphore` or a `void*` payload.
	namespace detail
	{
		struct TimelineSemaphoreData;
	} // namespace detail

	using TimelineSemaphore = detail::TimelineSemaphoreData;

	// Opaque typed handle returned by CreateTimelineSemaphore. The
	// engine treats it as the only valid way to refer to a semaphore.
	using TimelineSemaphoreHandle = TimelineSemaphore*;

	struct TimelineSemaphoreDesc
	{
		Device device = nullptr;
		std::uint64_t initialValue = 0;
		const char* debugName = nullptr;
	};

	// Create a timeline semaphore. The `device` field of `desc` must be
	// a valid `gpu::Device` obtained from `GpuDevice::GetDevice()`.
	// Returns `nullptr` on failure.
	[[nodiscard]] TimelineSemaphoreHandle CreateTimelineSemaphore(const TimelineSemaphoreDesc& desc) noexcept;

	// Wait for a timeline semaphore to reach the given value. The
	// `device` must be the same one passed to `CreateTimelineSemaphore`.
	// Returns true on success or timeout, false on error.
	[[nodiscard]] bool WaitTimelineSemaphore(Device device, TimelineSemaphoreHandle sem, std::uint64_t value) noexcept;

	// Destroy a timeline semaphore. The `device` must be the same one
	// passed to `CreateTimelineSemaphore`. The handle is invalid after
	// this call.
	void DestroyTimelineSemaphore(Device device, TimelineSemaphoreHandle sem) noexcept;
} // namespace aether::gpu
