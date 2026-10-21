#pragma once

#include <cstddef>
#include <glm/glm.hpp>

namespace aether
{
	struct MeshVertex
	{
		glm::vec3 position;
		glm::vec3 normal;
		glm::vec4 tangent;
		glm::vec2 uv;
		glm::vec2 uv2;
		glm::vec3 color;
		glm::uvec4 jointIndices{0u, 0u, 0u, 0u};
		glm::vec4 jointWeights{1.0f, 0.0f, 0.0f, 0.0f};
	};

	static_assert(sizeof(MeshVertex) == 100, "MeshVertex layout changed - update shaders/include/MeshVertex.slangh.");
	static_assert(offsetof(MeshVertex, position) == 0);
	static_assert(offsetof(MeshVertex, normal) == 12);
	static_assert(offsetof(MeshVertex, tangent) == 24);
	static_assert(offsetof(MeshVertex, uv) == 40);
	static_assert(offsetof(MeshVertex, uv2) == 48);
	static_assert(offsetof(MeshVertex, color) == 56);
	static_assert(offsetof(MeshVertex, jointIndices) == 68);
	static_assert(offsetof(MeshVertex, jointWeights) == 84);
} // namespace aether
