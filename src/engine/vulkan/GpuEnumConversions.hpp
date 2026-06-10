#pragma once

#include "vulkan/volk.hpp"

#include "gpu/GpuFormat.hpp"

namespace aether::gpu
{
	// Format conversions
	[[nodiscard]] VkFormat ToVk(Format format) noexcept;
	[[nodiscard]] Format FromVk(VkFormat format) noexcept;
	[[nodiscard]] bool IsDepthFormat(Format format) noexcept;
	[[nodiscard]] bool IsStencilFormat(Format format) noexcept;
	[[nodiscard]] bool IsBlockCompressed(Format format) noexcept;
	[[nodiscard]] std::uint32_t BytesPerPixel(Format format) noexcept;
} // namespace aether::gpu
