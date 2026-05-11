#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace aether
{
	// GPU-side material record stored in the MaterialBuffer.
	// Must stay binary-compatible with the GpuMaterial struct in gltf_mesh.slang.
	// Layout (80 bytes, no padding surprises because we avoid vec3 members):
	//
	//   offset  0 : vec4    baseColorFactor         (16)
	//   offset 16 : float   metallicFactor           ( 4)
	//   offset 20 : float   roughnessFactor          ( 4)
	//   offset 24 : float   occlusionStrength        ( 4)
	//   offset 28 : float   alphaCutoff              ( 4)
	//   offset 32 : vec4    emissiveFactor           (16)   xyz = emissive RGB, w
	//   unused offset 48 : uint32  flags                    ( 4) offset 52 : uint32
	//   albedoSlot               ( 4) offset 56 : uint32  normalSlot ( 4) offset 60
	//   : uint32  metallicRoughnessSlot    ( 4) offset 64 : uint32  occlusionSlot
	//   ( 4) offset 68 : uint32  emissiveSlot             ( 4) offset 72 : uint32
	//   _pad[2]                  ( 8) Total: 80 bytes

	struct GpuMaterial
	{
		static constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;
		static constexpr std::uint32_t kDoubleSided = 1u << 0;
		static constexpr std::uint32_t kAlphaBlend = 1u << 1;
		static constexpr std::uint32_t kAlphaMask = 1u << 2;

		glm::vec4 baseColorFactor{ 1.0f };
		float metallicFactor{ 1.0f };
		float roughnessFactor{ 1.0f };
		float occlusionStrength{ 1.0f };
		float alphaCutoff{ 0.5f };
		glm::vec4 emissiveFactor{ 0.0f }; // w unused
		std::uint32_t flags{ 0 };
		std::uint32_t albedoSlot{ kNoTexture };
		std::uint32_t normalSlot{ kNoTexture };
		std::uint32_t metallicRoughnessSlot{ kNoTexture };
		std::uint32_t occlusionSlot{ kNoTexture };
		std::uint32_t emissiveSlot{ kNoTexture };
		std::uint32_t _pad[2]{ 0, 0 };
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
	static_assert(offsetof(GpuMaterial, _pad) == 72);
} // namespace aether
