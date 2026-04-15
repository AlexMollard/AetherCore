#pragma once

#include <glm/glm.hpp>
#include <vector>

#include "AetherCore.hpp"
#include "AssetManager.hpp"
#include "Components.hpp"
#include "GraphicsPipeline.hpp"
#include "Material.hpp"
#include "Mesh.hpp"
#include "World.hpp"

namespace aether::ecs
{
	// Creates a single entity from an explicit mesh + material.
	inline aether::Entity SpawnMesh(aether::World& world, aether::GraphicsPipeline& pipeline, const aether::Mesh& mesh, aether::Material material, const glm::mat4& transform = glm::mat4(1.0f))
	{
		const aether::Entity entity = world.Create();
		world.EmplaceOrReplace<aether::PipelineComponent>(entity, aether::PipelineComponent{ .pipeline = &pipeline });
		world.EmplaceOrReplace<aether::MeshComponent>(entity, aether::MeshComponent{ .mesh = &mesh });
		world.EmplaceOrReplace<aether::MaterialComponent>(entity, aether::MaterialComponent{ .material = material });
		world.EmplaceOrReplace<aether::TransformComponent>(entity, aether::TransformComponent{ .localToWorld = transform });
		return entity;
	}

	// Spawns all primitives of a LoadedModel, tags every entity with the provided
	// tag list, and wires AnimatorComponent when the model has a skeleton.
	// Returns the number of entities spawned.
	template<typename... Tags>
	inline std::size_t SpawnModel(aether::World& world, aether::AssetManager& assets, aether::LoadedModel& model, aether::GraphicsPipeline& pipeline, float scale, Tags... tags)
	{
		const std::vector<aether::Entity> entities = assets.SpawnModel(model, pipeline, scale);
		for (const aether::Entity e: entities)
		{
			(world.EmplaceOrReplace<Tags>(e, tags), ...);
			if (model.animator)
				world.EmplaceOrReplace<aether::AnimatorComponent>(e, aether::AnimatorComponent{ .animator = &*model.animator });
		}
		return entities.size();
	}
} // namespace aether::ecs
