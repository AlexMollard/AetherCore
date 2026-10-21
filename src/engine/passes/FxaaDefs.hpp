#pragma once

#include <cstdint>
#include "gpu/GpuTypes.hpp"

namespace aether
{
	struct FxaaPushConstants
	{
		std::uint32_t ldrSlot = 0, fxaaEnabled = 0;
		float rcpWidth = 1, rcpHeight = 1, sharpness = 0;
		float outputRcpWidth = 1, outputRcpHeight = 1;
	};

	static_assert(sizeof(FxaaPushConstants) == 28);

	[[nodiscard]] inline bool NeedsScaledPresentation(gpu::Extent2D scene, gpu::Extent2D output, bool outputToTexture)
	{
		return !outputToTexture && (scene.width != output.width || scene.height != output.height);
	}
} // namespace aether
