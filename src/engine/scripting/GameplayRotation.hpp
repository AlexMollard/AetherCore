#pragma once

#include <glm/gtc/quaternion.hpp>
#include "scripting/GameplayAbi.h"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"

namespace aether::scripting
{
	inline AetherGameplayStatus SetGameplayEuler(World& world, Entity entity, const float* values, PhysicsSystem* physics)
	{
		auto* transform = world.TryGet<TransformComponent>(entity);
		if (!transform)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		glm::vec3 position{}, euler{}, scale{};
		DecomposeTRS(transform->localToWorld, position, euler, scale);
		ecs::SetWorldTransform(world, entity, ComposeTransform(position, {values[0], values[1], values[2]}, scale));
		if (auto* state = world.TryGet<PhysicsStateComponent>(entity))
		{
			const glm::quat rotation = glm::angleAxis(glm::radians(values[1]), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(values[0]), glm::vec3(1, 0, 0)) * glm::angleAxis(glm::radians(values[2]), glm::vec3(0, 0, 1));
			state->prevPosition = state->currPosition = position;
			state->prevRotation = state->currRotation = rotation;
			state->scale = glm::max(scale, glm::vec3(0.001f));
			if (const auto* body = world.TryGet<RigidBodyComponent>(entity); body && physics)
			{
				physics->SetPosition(body->body, position);
				physics->SetRotation(body->body, rotation);
			}
		}
		if (auto* character = world.TryGet<CharacterControllerComponent>(entity))
		{
			character->teleportPending = true;
		}
		return AE_GAMEPLAY_OK;
	}
}
