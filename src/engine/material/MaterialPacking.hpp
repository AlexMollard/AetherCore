#pragma once

#include "material/GpuMaterial.hpp"

namespace aether
{
	struct MaterialAsset;
	class TextureRegistry;

	// The single source of truth for CPU authoring -> GPU record conversion.
	// Resolves each TextureHandle to its live bindless heap index via the
	// registry; the GPU record still stores a raw uint32 slot (ABI frozen).
	[[nodiscard]] GpuMaterial PackMaterial(const MaterialAsset& asset, const TextureRegistry& textures);
} // namespace aether
