#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

#include "assets/AssetManager.hpp"
#include "scene/Components.hpp"
#include "scene/LoadedModel.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "mesh/Mesh.hpp"
#include "scene/World.hpp"

namespace aether
{
	class PipelineCache;
}

namespace aether::ecs
{
	inline aether::Entity SpawnMesh(aether::World& world, const aether::GraphicsPipeline* pipeline, const aether::Mesh& mesh, const glm::mat4& transform = glm::mat4(1.0f))
	{
		aether::Entity e = world.Create();
		world.EmplaceOrReplace<aether::PipelineComponent>(e, aether::PipelineComponent{.pipeline = pipeline});
		world.EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = &mesh});
		world.EmplaceOrReplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = transform});
		return e;
	}

	inline aether::Entity SpawnMesh(aether::World& world, const aether::Mesh& mesh, aether::MaterialRegistry& materials, aether::PipelineCache& pipelineCache, const aether::MaterialAsset& asset, const glm::mat4& transform = glm::mat4(1.0f))
	{
		aether::Entity e = world.Create();
		world.EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = &mesh});
		world.EmplaceOrReplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = transform});
		aether::MaterialSystem::AssignMaterial(world, e, materials, pipelineCache, asset);
		return e;
	}

	template<typename... Tags>
	inline std::size_t SpawnModel(aether::World& world, aether::AssetManager& assets, aether::LoadedModel& model, float scale, Tags... tags)
	{
		const std::vector<aether::Entity> entities = assets.SpawnModel(model, 0, scale);
		for (const aether::Entity e: entities)
		{
			(world.EmplaceOrReplace<Tags>(e, tags), ...);
		}
		return entities.size();
	}
} // namespace aether::ecs
