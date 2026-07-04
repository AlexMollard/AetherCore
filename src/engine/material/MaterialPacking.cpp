#include "material/MaterialPacking.hpp"

#include "material/MaterialAsset.hpp"

namespace aether
{
	GpuMaterial PackMaterial(const MaterialAsset& a)
	{
		GpuMaterial g{};
		g.baseColorFactor = a.baseColorFactor;
		g.metallicFactor = a.metallicFactor;
		g.roughnessFactor = a.roughnessFactor;
		g.occlusionStrength = a.occlusionStrength;
		g.alphaCutoff = a.alphaCutoff;
		g.emissiveFactor = glm::vec4(a.emissiveFactor, 0.0f);

		std::uint32_t flags = 0u;
		if (a.doubleSided) flags |= GpuMaterial::kDoubleSided;
		if (a.alphaBlend) flags |= GpuMaterial::kAlphaBlend;
		if (a.alphaMask) flags |= GpuMaterial::kAlphaMask;
		if (a.modulateVertexColor) flags |= GpuMaterial::kModulateVertexColor;
		g.flags = flags;

		g.albedoSlot = a.albedoSlot;
		g.normalSlot = a.normalSlot;
		g.metallicRoughnessSlot = a.metallicRoughnessSlot;
		g.occlusionSlot = a.occlusionSlot;
		g.emissiveSlot = a.emissiveSlot;
		return g;
	}
} // namespace aether
