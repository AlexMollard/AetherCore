#pragma once

#include <span>
#include <string>
#include "material/MaterialAsset.hpp"

namespace aether
{
	// Scene keys are shared by native and browser material resolution.
	inline void ApplyMaterialOverrides(MaterialAsset& onto, const MaterialAsset& from, std::span<const std::string> keys)
	{
		for (const auto& key: keys)
		{
			if (key == "base_color") { onto.baseColorFactor = from.baseColorFactor; }
			else if (key == "metallic") { onto.metallicFactor = from.metallicFactor; }
			else if (key == "roughness") { onto.roughnessFactor = from.roughnessFactor; }
			else if (key == "occlusion") { onto.occlusionStrength = from.occlusionStrength; }
			else if (key == "alpha_cutoff") { onto.alphaCutoff = from.alphaCutoff; }
			else if (key == "emissive") { onto.emissiveFactor = from.emissiveFactor; }
			else if (key == "double_sided") { onto.doubleSided = from.doubleSided; }
			else if (key == "alpha_blend") { onto.alphaBlend = from.alphaBlend; }
			else if (key == "alpha_mask") { onto.alphaMask = from.alphaMask; }
			else if (key == "vertex_color") { onto.modulateVertexColor = from.modulateVertexColor; }
			else if (key == "receive_shadows") { onto.receiveShadows = from.receiveShadows; }
			else if (key == "baked_lighting") { onto.bakedLighting = from.bakedLighting; }
			else if (key == "object_lit") { onto.objectLit = from.objectLit; }
			else if (key == "foliage") { onto.foliage = from.foliage; }
			else if (key == "sky") { onto.sky = from.sky; }
			else if (key == "light_shaft") { onto.lightShaft = from.lightShaft; }
			else if (key == "gem") { onto.gem = from.gem; }
			else if (key == "sheen") { onto.sheen = from.sheen; }
			else if (key == "water") { onto.water = from.water; }
			else if (key == "character") { onto.character = from.character; }
			else if (key == "additive_blend") { onto.additiveBlend = from.additiveBlend; }
			else if (key == "uv_scroll") { onto.uvScroll = from.uvScroll; }
		}
	}
}
