#pragma once

#include <glm/glm.hpp>
#include <vector>

#include "animation/AnimationDatabase.hpp"
#include "material/MaterialAsset.hpp"
#include "material/Texture.hpp"
#include "mesh/Mesh.hpp"

namespace aether
{
	struct LoadedModelPrimitive
	{
		Mesh mesh;
		// Authoring data only; a registry handle is acquired per spawned entity.
		// hasMaterial=false spawns without a MaterialComponent (shader falls back
		// to vertex colour), matching primitives with no glTF material.
		MaterialAsset material{};
		bool hasMaterial = false;
		glm::mat4 localTransform{1.0f};
		std::int32_t skinIndex = -1;
	};

	struct LoadedModel
	{
		std::vector<Texture> textures;
		std::vector<LoadedModelPrimitive> primitives;
		AnimationDatabase animationDb;
	};
} // namespace aether
