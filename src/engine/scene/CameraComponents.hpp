#pragma once

#include <cmath>
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

	// Turns a camera entity into an orbit (third-person) camera: CameraSystem
	// recomputes the entity's TransformComponent from these params every frame, so
	// the eye circles `target` at `distance` on the (yaw, pitch) sphere. This is
	// how scripted follow-cameras stay first-class entities in the hierarchy - it
	// replaces the old raw CameraManager "Orbit mode". Scripts mutate the fields
	// through the Camera interop (SetTarget/SetOrbital); the engine owns the math.
	struct OrbitCameraComponent
	{
		glm::vec3 target{0.0f}; // world-space point the camera looks at and circles
		float yaw = 0.0f;       // degrees around +Y (0 => camera on the +Z side)
		float pitch = 20.0f;    // degrees above the horizon (clamped by the driver)
		float distance = 10.0f; // metres from the target
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

		// Pitch clamp shared by the orbit driver and the interop, so a script can't
		// flip the camera over the pole. Matches the retired Camera::kPitchLimit.
		inline constexpr float kOrbitPitchLimit = 89.0f;

		// World matrix for an orbit camera: eye = target + spherical(yaw, pitch,
		// distance), looking back at the target. The spherical convention matches
		// the retired Camera "Orbit mode" so existing yaw/pitch/distance values keep
		// their framing. CameraSystem writes this into the entity's TransformComponent
		// each frame; the same pose is what GetForward/GetRight report.
		inline glm::mat4 OrbitCameraMatrix(const glm::vec3& target, float yaw, float pitch, float distance)
		{
			const float oy = glm::radians(yaw);
			const float op = glm::radians(glm::clamp(pitch, -kOrbitPitchLimit, kOrbitPitchLimit));
			const float d = glm::max(0.1f, distance);
			const glm::vec3 offset{
			        d * std::cos(op) * std::sin(oy),
			        d * std::sin(op),
			        d * std::cos(op) * std::cos(oy),
			};
			const glm::vec3 eye = target + offset;
			return CameraAimMatrix(eye, target - eye);
		}

		// Spawn a camera entity that orbits `target`. It appears in the hierarchy
		// like any other entity and, once tagged via SetMainCameraEntity, drives the
		// scene view. This is the entity-first replacement for the old scripted
		// CameraManager::Create(Orbit) path.
		inline Entity CreateOrbitCameraEntity(World& world, const glm::vec3& target, float yaw, float pitch, float distance, const CameraComponent& camera = {}, std::string name = "Camera")
		{
			const glm::mat4 pose = OrbitCameraMatrix(target, yaw, pitch, distance);
			const Entity e = CreateCameraEntity(world, glm::vec3(pose[3]), -glm::vec3(pose[2]), camera, std::move(name));
			world.Emplace<OrbitCameraComponent>(e, OrbitCameraComponent{.target = target, .yaw = yaw, .pitch = pitch, .distance = distance});
			return e;
		}
	} // namespace ecs
} // namespace aether
