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
		// PS2 foliage/cutout card: wrapped diffuse, casts offset from the sun, no contact shadows.
		static constexpr std::uint32_t kFoliage = 1u << 5;
		// PS2 prelit geometry: vertex colour is the lighting, shadow maps pull it to the ambient.
		static constexpr std::uint32_t kBakedLighting = 1u << 6;
		// PS2 object lighting: lit by the scene's object light records (ambient + two
		// directional) like the GS lit everything that was not prelit scenery.
		static constexpr std::uint32_t kObjectLit = 1u << 7;
		// Additive blend (Cs*As + Cd). Pipeline state; gltf_mesh reads it (kFlagAdditiveBlend) only to
		// keep the aerial haze off additive surfaces. Also carried so MaterialRegistry::TryDescribe round-trips it.
		static constexpr std::uint32_t kAdditiveBlend = 1u << 8;
		// Skydome surface: the vertex shaders put it at infinite depth (clip z = w). Mirrors
		// kFlagSky in GpuMaterial.slangh.
		static constexpr std::uint32_t kSky = 1u << 9;
		// Volumetric light shaft card (Twinsanity god rays): the shader fades it at its sheet's
		// edges (TEXCOORD_1), edge-on, near the camera and where it meets geometry, and makes the
		// rays breathe in place. Mirrors kFlagLightShaft in GpuMaterial.slangh.
		static constexpr std::uint32_t kLightShaft = 1u << 10;
		// Twinsanity gem: faceted crystal shading (flat facets, fresnel rim, fake refraction,
		// facet sparkle) replacing the object lighting. Mirrors kFlagGem in GpuMaterial.slangh.
		static constexpr std::uint32_t kGem = 1u << 11;
		// Collectible sheen (wumpa fruit): object lighting plus the shared PickupSheen rim and
		// highlight. Mirrors kFlagSheen in GpuMaterial.slangh.
		static constexpr std::uint32_t kSheen = 1u << 12;
		// Water look (sea, shore, pools, waterfalls): depth tint, screen-space reflection, ripple
		// normals and sun glints. Mirrors kFlagWater in GpuMaterial.slangh.
		static constexpr std::uint32_t kWater = 1u << 13;
		// Character look (Crash, Aku Aku): wrapped key, rim, eye catchlight and texture relief on
		// top of the object lighting. Mirrors kFlagCharacter in GpuMaterial.slangh.
		static constexpr std::uint32_t kCharacter = 1u << 14;

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
