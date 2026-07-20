#pragma once

#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether
{
	// propagation carries them) and serialize like any other entity.
	struct PointLightComponent
	{
		glm::vec3 color{1.0f};
		float intensity = 20.0f;
		float radius = 15.0f;
		bool castsShadow = false;
	};

	struct SpotLightComponent
	{
		glm::vec3 color{1.0f};
		float intensity = 30.0f;
		float radius = 30.0f;
		float innerAngleRad = 0.35f;
		float outerAngleRad = 0.60f;
		bool castsShadow = false;
	};

	// Animated sun/sky environment driver. Scene content, not a global: the
	// DayNightSystem drives the renderer from the first enabled instance and
	// leaves the authored environment untouched when no entity carries one, so
	// 2D scenes (no Lighting3D feature) never fight it. Time state lives here
	// so it serializes and play-restores like any other authored data.
	struct DayNightComponent
	{
		bool animate = true;
		float timeOfDayHours = 6.0f;             // wraps [0, 24)
		float timeSpeedSecondsPerSecond = 60.0f; // simulated seconds per real second
	};

	namespace ecs
	{
		inline glm::mat4 LightAimMatrix(const glm::vec3& position, const glm::vec3& direction)
		{
			const glm::vec3 dir = glm::normalize(direction);
			const glm::vec3 up = glm::abs(glm::dot(dir, glm::vec3(0, 1, 0))) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			glm::mat4 m = glm::mat4_cast(glm::quatLookAt(dir, up));
			m[3] = glm::vec4(position, 1.0f);
			return m;
		}

		inline Entity CreatePointLightEntity(World& world, const glm::vec3& position, const PointLightComponent& light, std::string name = "Point Light")
		{
			const Entity e = world.Create();
			world.Emplace<NameComponent>(e, NameComponent{.name = std::move(name)});
			glm::mat4 m{1.0f};
			m[3] = glm::vec4(position, 1.0f);
			world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = m});
			world.Emplace<PointLightComponent>(e, light);
			return e;
		}

		inline Entity CreateSpotLightEntity(World& world, const glm::vec3& position, const glm::vec3& direction, const SpotLightComponent& light, std::string name = "Spot Light")
		{
			const Entity e = world.Create();
			world.Emplace<NameComponent>(e, NameComponent{.name = std::move(name)});
			world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = LightAimMatrix(position, direction)});
			world.Emplace<SpotLightComponent>(e, light);
			return e;
		}
	} // namespace ecs
} // namespace aether
