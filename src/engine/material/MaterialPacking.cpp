#include "material/MaterialPacking.hpp"

#include "material/MaterialAsset.hpp"
#include "material/TextureRegistry.hpp"

namespace aether
{
	GpuMaterial PackMaterial(const MaterialAsset& a, const TextureRegistry& textures)
	{
		GpuMaterial g{};
		g.baseColorFactor = a.baseColorFactor;
		g.metallicFactor = a.metallicFactor;
		g.roughnessFactor = a.roughnessFactor;
		g.occlusionStrength = a.occlusionStrength;
		g.alphaCutoff = a.alphaCutoff;
		g.emissiveFactor = glm::vec4(a.emissiveFactor, 0.0f);

		std::uint32_t flags = 0u;
		if (a.doubleSided)
		{
			flags |= GpuMaterial::kDoubleSided;
		}
		if (a.alphaBlend)
		{
			flags |= GpuMaterial::kAlphaBlend;
		}
		if (a.alphaMask)
		{
			flags |= GpuMaterial::kAlphaMask;
		}
		if (a.modulateVertexColor)
		{
			flags |= GpuMaterial::kModulateVertexColor;
		}
		if (!a.receiveShadows)
		{
			flags |= GpuMaterial::kNoReceiveShadows;
		}
		if (a.foliage)
		{
			flags |= GpuMaterial::kFoliage;
		}
		g.flags = flags;
		g.uvScroll = a.uvScroll;

		auto slotFor = [&textures](const TextureHandle h) -> std::uint32_t
		{
			return h.IsValid() ? textures.ResolveSlot(h) : GpuMaterial::kNoTexture;
		};

		g.albedoSlot = slotFor(a.albedoTex);
		g.normalSlot = slotFor(a.normalTex);
		g.metallicRoughnessSlot = slotFor(a.metallicRoughnessTex);
		g.occlusionSlot = slotFor(a.occlusionTex);
		g.emissiveSlot = slotFor(a.emissiveTex);
		return g;
	}
} // namespace aether
