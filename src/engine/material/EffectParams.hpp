#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace aether
{
	// Per-entity animated effect inputs. Mutable per-entity state (NOT content-
	// addressed, unlike GpuMaterial) stored in EffectParamBuffer, one slot per
	// effect entity. Replaces the PBR-field smuggling in the old plasma path
	// (tint<-emissive, speed<-metallic, scale<-roughness, intensity<-occlusion).
	// Must stay binary-compatible with EffectParams in shaders/include/EffectParams.slangh.
	struct EffectParams
	{
		glm::vec4 tint{1.0f};   // offset 0
		float speed = 1.0f;     // offset 16
		float scale = 1.0f;     // offset 20
		float intensity = 1.0f; // offset 24
		std::uint32_t _pad = 0; // offset 28
	};

	static_assert(sizeof(EffectParams) == 32, "EffectParams size changed - update shaders/include/EffectParams.slangh.");
	static_assert(offsetof(EffectParams, tint) == 0);
	static_assert(offsetof(EffectParams, speed) == 16);
	static_assert(offsetof(EffectParams, scale) == 20);
	static_assert(offsetof(EffectParams, intensity) == 24);
	static_assert(offsetof(EffectParams, _pad) == 28);
} // namespace aether
