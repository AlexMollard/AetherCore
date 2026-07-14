#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace aether
{
	// Must stay binary-compatible with EffectParams in shaders/include/EffectParams.slangh.
	struct EffectParams
	{
		glm::vec4 tint{1.0f};
		float speed = 1.0f;
		float scale = 1.0f;
		float intensity = 1.0f;
		std::uint32_t _pad = 0;
	};

	static_assert(sizeof(EffectParams) == 32, "EffectParams size changed - update shaders/include/EffectParams.slangh.");
	static_assert(offsetof(EffectParams, tint) == 0);
	static_assert(offsetof(EffectParams, speed) == 16);
	static_assert(offsetof(EffectParams, scale) == 20);
	static_assert(offsetof(EffectParams, intensity) == 24);
	static_assert(offsetof(EffectParams, _pad) == 28);
} // namespace aether
