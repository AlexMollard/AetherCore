#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether
{
	// A camera that lives on an entity. Like the punctual lights, the pose comes
	// from the entity's TransformComponent: the camera looks along the entity's
	// LOCAL -Z (camera-forward convention), so rotating the entity aims the view.
	// CameraSystem mirrors each CameraComponent into a Manual camera in the
	// CameraManager pool every frame, so the render path (which reads
	// CameraManager) can look through it and gizmos can draw its frustum.
	struct CameraComponent
	{
		float fovDegrees = 60.0f;
		float nearPlane = 0.1f;
		float farPlane = 1000.0f;

		// Runtime-only: id of the backing CameraManager camera CameraSystem keeps
		// in sync (0 = not created yet). Never serialized - it is re-established on
		// load and can change between runs.
		std::uint32_t backingCamera = 0;
	};

	// Tags the single entity whose CameraComponent drives the main scene view
	// while Playing. Set through ecs::SetMainCameraEntity, which enforces the
	// "only one" invariant by clearing the tag from every other entity.
	struct MainCameraComponent
	{
		bool dummy = true;
	};

	namespace ecs
	{
		// World matrix at `position` whose local -Z points along `direction` -
		// same convention as LightAimMatrix, kept separate so intent reads clearly.
		inline glm::mat4 CameraAimMatrix(const glm::vec3& position, const glm::vec3& direction)
		{
			const glm::vec3 dir = glm::normalize(direction);
			const glm::vec3 up = glm::abs(glm::dot(dir, glm::vec3(0, 1, 0))) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			glm::mat4 m = glm::mat4_cast(glm::quatLookAt(dir, up));
			m[3] = glm::vec4(position, 1.0f);
			return m;
		}

		// The entity currently tagged as the main camera, or an invalid entity.
		inline Entity GetMainCameraEntity(const World& world)
		{
			Entity found{};
			world.View<const MainCameraComponent>().each(
			        [&](entt::entity e, const MainCameraComponent&)
			        {
				        if (!found.IsValid())
				        {
					        found = World::FromEntt(e);
				        }
			        });
			return found;
		}

		// Makes `entity` the sole main camera: clears MainCameraComponent from any
		// other entity first, then tags this one. No-op if the entity is invalid.
		inline void SetMainCameraEntity(World& world, Entity entity)
		{
			auto& reg = world.GetRegistry();
			// Collect first: removing while iterating the same component view is unsafe.
			std::vector<entt::entity> existing;
			for (const entt::entity e: reg.view<MainCameraComponent>())
			{
				existing.push_back(e);
			}
			for (const entt::entity e: existing)
			{
				reg.remove<MainCameraComponent>(e);
			}
			if (entity.IsValid() && reg.valid(World::ToEntt(entity)))
			{
				world.EmplaceOrReplace<MainCameraComponent>(entity, MainCameraComponent{});
			}
		}

		inline Entity CreateCameraEntity(World& world, const glm::vec3& position, const glm::vec3& direction, const CameraComponent& camera = {}, std::string name = "Camera")
		{
			const Entity e = world.Create();
			world.Emplace<NameComponent>(e, NameComponent{.name = std::move(name)});
			world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = CameraAimMatrix(position, direction)});
			world.Emplace<CameraComponent>(e, camera);
			return e;
		}
	} // namespace ecs
} // namespace aether
