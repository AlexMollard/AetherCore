#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "animation/AnimationDatabase.hpp"
#include "material/MaterialAsset.hpp"
#include "mesh/Mesh.hpp"

namespace aether
{
	struct LoadedModelPrimitive
	{
		Mesh mesh;
		MaterialAsset material{};
		bool hasMaterial = false;
		glm::mat4 localTransform{1.0f};
		std::int32_t skinIndex = -1;
		std::string primitiveKey;
		std::string legacyGeometryKey;
	};

	struct LoadedModel
	{
		std::vector<LoadedModelPrimitive> primitives;
		AnimationDatabase animationDb;
	};
} // namespace aether
