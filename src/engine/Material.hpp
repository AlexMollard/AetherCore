#pragma once

#include <cstdint>
#include <limits>

#include <glm/glm.hpp>

namespace aether
{
	// Describes the shading properties of a surface.
	// Bindless slot indices reference images registered in BindlessManager.
	// A slot of kNoTexture causes the shader to fall back to vertex colour.
	struct Material
	{
		static constexpr std::uint32_t kNoTexture = std::numeric_limits<std::uint32_t>::max();

		// ── PBR base factors ──────────────────────────────────────────────
		glm::vec4 baseColorFactor  { 1.0f };
		float     metallicFactor   { 1.0f };
		float     roughnessFactor  { 1.0f };
		float     occlusionStrength{ 1.0f };
		float     alphaCutoff      { 0.5f };
		glm::vec3 emissiveFactor   { 0.0f };

		// ── Flags ─────────────────────────────────────────────────────────
		bool doubleSided = false;
		bool alphaBlend  = false;
		bool alphaMask   = false;

		// ── Bindless texture slots ─────────────────────────────────────────
		std::uint32_t albedoSlot            = kNoTexture;
		std::uint32_t normalSlot            = kNoTexture;
		std::uint32_t metallicRoughnessSlot = kNoTexture;
		std::uint32_t occlusionSlot         = kNoTexture;
		std::uint32_t emissiveSlot          = kNoTexture;

		// ── GPU material buffer slot ───────────────────────────────────────
		// Filled by AetherCore::RegisterMaterial().
		// Used as the materialIndex push constant for each draw call.
		std::uint32_t materialSlot = kNoTexture;
	};
}
