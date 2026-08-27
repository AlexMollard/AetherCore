#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "camera/CameraProjection.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether
{
	enum class CameraBackground : std::uint8_t
	{
		SolidColour = 0,
		Gradient    = 1,
		SkyGradient = 2, // procedural sky (renderer environment / DayNight entity)
	};

	struct GradientStop
	{
		glm::vec3 colour{0.0f};
		float position = 0.0f; // 0..1 along the gradient axis
	};

	struct CameraComponent
	{
		CameraProjection projection = CameraProjection::Perspective;
		float fovDegrees = 60.0f;
		float orthographicHeight = 10.0f;
		float nearPlane = 0.1f;
		float farPlane = 1000.0f;

		// Scene background policy, owned by the main camera. Solid/Gradient are
		// composited WYSIWYG in the tonemap pass; SkyGradient uses the procedural
		// sky gradient (renderer environment / DayNight entity).
		CameraBackground background = CameraBackground::SkyGradient;
		glm::vec3 clearColor{0.10f, 0.10f, 0.12f}; // SolidColour mode
		std::vector<GradientStop> gradientStops{   // Gradient mode (>=2)
		        GradientStop{{0.05f, 0.06f, 0.10f}, 0.0f},
		        GradientStop{{0.02f, 0.02f, 0.03f}, 1.0f},
		};
		float gradientAngleDegrees = 0.0f; // 0 = top->bottom

		// Depth of field, as a lens rather than a distance blur. A lens focuses one plane
		// and everything else lands as a disc; how big that disc grows is set by the
		// aperture. These are the numbers a photographer reaches for, not a blur radius
		// that means nothing outside this renderer.
		bool depthOfField = false;
		// Distance to the plane that comes out sharp, in world units.
		float focusDistance = 10.0f;
		// f-number. Small is a wide aperture and a shallow slice in focus.
		float aperture = 5.6f;
		// Lens focal length in millimetres, for the blur only - the field of view above
		// still frames the shot.
		//
		// On a real camera these are one number, and deriving one from the other was the
		// first thing tried here. It does not survive contact with a game: a 60 degree
		// field of view is a 21 mm lens, and a 21 mm lens is so deep in focus that even
		// f/1.4 leaves a whole scene sharp. Field of view is a gameplay decision, so tying
		// the lens to it means a normal game camera can never have shallow focus.
		float focalLengthMm = 50.0f;

		// in sync (0 = not created yet). Never serialized - it is re-established on
		std::uint32_t backingCamera = 0;
	};

	struct OrbitCameraComponent
	{
		glm::vec3 target{0.0f};
		float yaw = 0.0f;
		float pitch = 20.0f;
		float distance = 10.0f;
	};

	// "only one" invariant by clearing the tag from every other entity.
	struct MainCameraComponent
	{
		bool dummy = true;
	};

	namespace ecs
	{
		inline glm::mat4 CameraAimMatrix(const glm::vec3& position, const glm::vec3& direction)
		{
			const glm::vec3 dir = glm::normalize(direction);
			const glm::vec3 up = glm::abs(glm::dot(dir, glm::vec3(0, 1, 0))) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			glm::mat4 m = glm::mat4_cast(glm::quatLookAt(dir, up));
			m[3] = glm::vec4(position, 1.0f);
			return m;
		}

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

		inline constexpr float kOrbitPitchLimit = 89.0f;

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

		inline Entity CreateOrbitCameraEntity(World& world, const glm::vec3& target, float yaw, float pitch, float distance, const CameraComponent& camera = {}, std::string name = "Camera")
		{
			const glm::mat4 pose = OrbitCameraMatrix(target, yaw, pitch, distance);
			const Entity e = CreateCameraEntity(world, glm::vec3(pose[3]), -glm::vec3(pose[2]), camera, std::move(name));
			world.Emplace<OrbitCameraComponent>(e, OrbitCameraComponent{.target = target, .yaw = yaw, .pitch = pitch, .distance = distance});
			return e;
		}
	} // namespace ecs
} // namespace aether
