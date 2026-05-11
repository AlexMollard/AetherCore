#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

#include "scene/AetherCore.hpp"
#include "utils/AssetManager.hpp"
#include "scene/Components.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/Material.hpp"
#include "mesh/Mesh.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	// Creates a single entity from an explicit mesh + material.
	// Returns an EntityHandle - implicitly converts to Entity for backward compat.
	inline aether::EntityHandle SpawnMesh(aether::World& world, aether::GraphicsPipeline& pipeline, const aether::Mesh& mesh, aether::Material material, const glm::mat4& transform = glm::mat4(1.0f))
	{
		return world.Spawn()
		        .AddOrReplace<aether::PipelineComponent>(aether::PipelineComponent{ .pipeline = &pipeline })
		        .AddOrReplace<aether::MeshComponent>(aether::MeshComponent{ .mesh = &mesh })
		        .AddOrReplace<aether::MaterialComponent>(aether::MaterialComponent{ .material = material })
		        .AddOrReplace<aether::TransformComponent>(aether::TransformComponent{ .localToWorld = transform });
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
			{
				world.EmplaceOrReplace<aether::AnimatorComponent>(e, aether::AnimatorComponent{ .animator = &*model.animator, .animationDb = model.animationDb.IsValid() ? &model.animationDb : nullptr, .heroCharacter = true, .lodTier = 0 });
			}
		}
		return entities.size();
	}

	// Wires SkinComponent and AnimatorComponent onto a set of already-spawned
	// primitive entities, matching each entity to its skin by primitive index.
	// animator must have stable storage (e.g. inside a pre-reserved vector).
	inline void LinkModelAnimator(aether::World& world, std::span<const aether::Entity> entities, const aether::LoadedModel& model, aether::ModelAnimator& animator, bool heroCharacter = false, std::uint8_t lodTier = 2)
	{
		for (std::size_t p = 0; p < entities.size() && p < model.primitives.size(); ++p)
		{
			const aether::Entity e = entities[p];
			const std::int32_t skinIdx = model.primitives[p].skinIndex;
			if (skinIdx >= 0)
			{
				const VkDeviceAddress addr = animator.GetSkinBufferAddr(skinIdx);
				const std::uint32_t joints = animator.GetSkinJointCount(skinIdx);
				if (addr != 0 && joints > 0)
				{
					world.EmplaceOrReplace<aether::SkinComponent>(e, aether::SkinComponent{ .sourceSkinBufferAddr = addr, .jointCount = joints });
				}
			}
			world.EmplaceOrReplace<aether::AnimatorComponent>(e, aether::AnimatorComponent{ .animator = &animator, .heroCharacter = heroCharacter, .lodTier = lodTier });
		}
	}

	// Spawns all primitives of model, clones the animator into outAnimators, and
	// wires SkinComponent + AnimatorComponent onto every spawned entity.
	// outAnimators MUST be pre-reserved before entering any spawn loop to prevent
	// reallocation (which would invalidate the AnimatorComponent raw pointers).
	// Returns the spawned entities. The cloned animator is at outAnimators.back().
	[[nodiscard]] inline std::vector<aether::Entity> SpawnModelInstance(aether::World& world, aether::AssetManager& assets, aether::LoadedModel& model, aether::GraphicsPipeline& pipeline, float scale, std::vector<aether::ModelAnimator>& outAnimators, bool heroCharacter = false, std::uint8_t lodTier = 2)
	{
		std::vector<aether::Entity> entities = assets.SpawnModel(model, pipeline, scale);
		if (model.animator)
		{
			outAnimators.push_back(model.animator->Clone());
			LinkModelAnimator(world, entities, model, outAnimators.back(), heroCharacter, lodTier);
		}
		return entities;
	}
} // namespace aether::ecs
