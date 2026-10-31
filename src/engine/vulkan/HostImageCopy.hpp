#pragma once

#include "vulkan/volk.hpp"

namespace aether::vulkan
{
	// Configured before upload workers start and cleared after they stop. Reads from
	// CanUseHostImageCopy are otherwise immutable for the lifetime of the device.
	void SetHostImageCopySupport(VkPhysicalDevice physicalDevice, bool enabled) noexcept;
	[[nodiscard]] bool HostImageCopyDisabledByEnvironment() noexcept;
}
