#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <limits>

namespace aether
{
	// Pure authoring description of a surface. No GPU slot — that lives in the
	// MaterialRegistry. Packed into a GpuMaterial by PackMaterial().
	struct MaterialAsset
	{
		static constexpr std::uint32_t kNoTexture = std::numeric_limits<std::uint32_t>::max();

		glm::vec4 baseColorFactor{1.0f};
		float metallicFactor{0.0f};
		float roughnessFactor{0.5f};
		float occlusionStrength{1.0f};
		float alphaCutoff{0.5f};
		glm::vec3 emissiveFactor{0.0f};

		bool doubleSided = false;
		bool alphaBlend = false;
		bool alphaMask = false;
		// When true, the shader multiplies base color by the mesh's vertex color
		// (used by primitive meshes that carry meaningful vertex colors).
		bool modulateVertexColor = false;

		std::uint32_t albedoSlot = kNoTexture;
		std::uint32_t normalSlot = kNoTexture;
		std::uint32_t metallicRoughnessSlot = kNoTexture;
		std::uint32_t occlusionSlot = kNoTexture;
		std::uint32_t emissiveSlot = kNoTexture;
	};
} // namespace aether
