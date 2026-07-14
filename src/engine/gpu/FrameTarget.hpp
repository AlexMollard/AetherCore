#pragma once

#include <cstdint>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
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
