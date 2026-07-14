#pragma once

#include "material/GpuMaterial.hpp"

namespace aether
{
	struct MaterialAsset;
	class TextureRegistry;

	// registry; the GPU record still stores a raw uint32 slot (ABI frozen).
	[[nodiscard]] GpuMaterial PackMaterial(const MaterialAsset& asset, const TextureRegistry& textures);
} // namespace aether
