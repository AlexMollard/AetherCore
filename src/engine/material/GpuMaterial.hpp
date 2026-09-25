#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace aether
{
	// Must stay binary-compatible with the GpuMaterial struct in gltf_mesh.slang.

	struct GpuMaterial
	{
		static constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;
		static constexpr std::uint32_t kDoubleSided = 1u << 0;
		static constexpr std::uint32_t kAlphaBlend = 1u << 1;
		static constexpr std::uint32_t kAlphaMask = 1u << 2;
		static constexpr std::uint32_t kModulateVertexColor = 1u << 3;
		static constexpr std::uint32_t kNoReceiveShadows = 1u << 4;
		// PS2 foliage/cutout card: wrapped diffuse, no shadow-map receive, no shadow casting.
		static constexpr std::uint32_t kFoliage = 1u << 5;

		glm::vec4 baseColorFactor{1.0f};
		float metallicFactor{1.0f};
		float roughnessFactor{1.0f};
		float occlusionStrength{1.0f};
		float alphaCutoff{0.5f};
		glm::vec4 emissiveFactor{0.0f};
		std::uint32_t flags{0};
		std::uint32_t albedoSlot{kNoTexture};
		std::uint32_t normalSlot{kNoTexture};
		std::uint32_t metallicRoughnessSlot{kNoTexture};
		std::uint32_t occlusionSlot{kNoTexture};
		std::uint32_t emissiveSlot{kNoTexture};
		// UV scroll velocity, UV units per second, added to the sampled UV scaled by frame time.
		glm::vec2 uvScroll{0.0f, 0.0f};

		bool operator==(const GpuMaterial&) const = default;
	};

	static_assert(sizeof(GpuMaterial) == 80, "GpuMaterial size changed - update the Slang struct in gltf_mesh.slang.");
	static_assert(offsetof(GpuMaterial, baseColorFactor) == 0);
	static_assert(offsetof(GpuMaterial, metallicFactor) == 16);
	static_assert(offsetof(GpuMaterial, roughnessFactor) == 20);
	static_assert(offsetof(GpuMaterial, occlusionStrength) == 24);
	static_assert(offsetof(GpuMaterial, alphaCutoff) == 28);
	static_assert(offsetof(GpuMaterial, emissiveFactor) == 32);
	static_assert(offsetof(GpuMaterial, flags) == 48);
	static_assert(offsetof(GpuMaterial, albedoSlot) == 52);
	static_assert(offsetof(GpuMaterial, normalSlot) == 56);
	static_assert(offsetof(GpuMaterial, metallicRoughnessSlot) == 60);
	static_assert(offsetof(GpuMaterial, occlusionSlot) == 64);
	static_assert(offsetof(GpuMaterial, emissiveSlot) == 68);
	static_assert(offsetof(GpuMaterial, uvScroll) == 72);
} // namespace aether
