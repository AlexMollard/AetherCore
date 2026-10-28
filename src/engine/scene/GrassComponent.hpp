#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

namespace aether
{
	struct GrassComponent
	{
		bool enabled = true;
		std::string surfaceMesh;
		std::string exclusionMesh;
		glm::vec2 regionMin{-10.0f, -10.0f};
		glm::vec2 regionMax{10.0f, 10.0f};
		float spacing = 0.14f;
		float chunkSize = 4.0f;
		glm::vec2 heightRange{0.35f, 0.85f};
		float bladeWidth = 0.035f;
		glm::vec2 lodDistances{10.0f, 26.0f};
		float drawDistance = 44.0f;
		float windStrength = 1.0f;
		bool surfaceMask = false;
		std::uint32_t seed = 1;
		glm::vec3 baseColor{0.18f, 0.29f, 0.07f};
		glm::vec3 tipColor{0.50f, 0.62f, 0.22f};
	};
}
