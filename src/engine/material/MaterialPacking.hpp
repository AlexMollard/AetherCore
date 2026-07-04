#pragma once

#include "material/GpuMaterial.hpp"

namespace aether
{
	struct MaterialAsset;

	// The single source of truth for CPU authoring -> GPU record conversion.
	[[nodiscard]] GpuMaterial PackMaterial(const MaterialAsset& asset);
} // namespace aether
