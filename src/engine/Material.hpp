#pragma once

#include <cstdint>
#include <limits>

#include <glm/glm.hpp>

namespace meow
{
	// Describes the shading properties of a surface.
	// Bindless slot indices reference images registered in BindlessManager.
	// A slot of kNoTexture causes the shader to fall back to vertex colour.
	struct Material
	{
		static constexpr std::uint32_t kNoTexture = std::numeric_limits<std::uint32_t>::max();

		// Albedo / base-colour texture.  SRGB image converted to linear by the hardware when sampled.
		std::uint32_t albedoSlot = kNoTexture;

		// Uniform tint multiplied into the final albedo (linear, default = white).
		glm::vec4 albedoTint{ 1.0f };
	};
}
