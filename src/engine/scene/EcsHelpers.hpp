#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "assets/AssetManager.hpp"
#include "scene/Components.hpp"
#include "scene/LoadedModel.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/Material.hpp"
#include "mesh/Mesh.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	// Creates a single entity from an explicit mesh + material.
	inline aether::EntityHandle SpawnMesh(aether::World& world, aether::GraphicsPipeline& pipeline, const aether::Mesh& mesh, aether::Material material, const glm::mat4& transform = glm::mat4(1.0f))
	{
		return world.Spawn()
		        .AddOrReplace<aether::PipelineComponent>(aether::PipelineComponent{.pipeline = &pipeline})
		        .AddOrReplace<aether::MeshComponent>(aether::MeshComponent{.mesh = &mesh})
		        .AddOrReplace<aether::MaterialComponent>(aether::MaterialComponent{.material = material})
		        .AddOrReplace<aether::TransformComponent>(aether::TransformComponent{.localToWorld = transform});
	}

	// Spawns all primitives of a LoadedModel and tags every entity with the provided
	// tag list. SkinnedMeshComponent is attached automatically by AssetManager::SpawnModel
	// when the model has animation data.
	template<typename... Tags>
	inline std::size_t SpawnModel(aether::World& world, aether::AssetManager& assets, aether::LoadedModel& model, aether::GraphicsPipeline& pipeline, float scale, Tags... tags)
	{
		const std::vector<aether::Entity> entities = assets.SpawnModel(model, pipeline, 0, scale);
		for (const aether::Entity e: entities)
		{
			(world.EmplaceOrReplace<Tags>(e, tags), ...);
		}
		return entities.size();
	}
} // namespace aether::ecs
