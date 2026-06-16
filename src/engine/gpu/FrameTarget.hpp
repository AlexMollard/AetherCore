#pragma once

#include <cstdint>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	// Per-frame swapchain handles supplied to RenderGraph::Execute by AetherCore.
	// Uses opaque void* handles (same pattern as gpu::CommandList) so the header
	// stays Vulkan-free. The backend (RenderGraphStorage / vulkan layer) casts
	// these back to VkImage/VkImageView at execution time.
	struct FrameTarget
	{
		void* colorImage = nullptr;
		void* colorView = nullptr;
		void* depthImage = nullptr;
		void* depthView = nullptr;
		gpu::Format colorFormat = gpu::Format::Undefined;
		gpu::Format depthFormat = gpu::Format::Undefined;
		gpu::Extent2D extent;
	};
} // namespace aether
