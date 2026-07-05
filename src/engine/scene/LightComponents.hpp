#pragma once

#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether
{
	// Punctual lights as plain components: position comes from the entity's
	// TransformComponent, so lights select, gizmo-move, parent (delta
	// propagation carries them) and serialize like any other entity.
	// LightSystem republishes the live set to the renderer every frame.
	struct PointLightComponent
	{
		glm::vec3 color{1.0f};
		float intensity = 20.0f;
		float radius = 15.0f;
		bool castsShadow = false;
	};

	// A spot aims along the entity's LOCAL -Z (camera-forward convention):
	// rotate the entity to aim the cone.
	struct SpotLightComponent
	{
		glm::vec3 color{1.0f};
		float intensity = 30.0f;
		float radius = 30.0f;
		float innerAngleRad = 0.35f;
		float outerAngleRad = 0.60f;
		bool castsShadow = false;
	};

	namespace ecs
	{
		// World matrix at `position` whose local -Z points along `direction` -
		// bridges direction-vector light data (das bindings, legacy scene
		// records) onto the entity convention.
		inline glm::mat4 LightAimMatrix(const glm::vec3& position, const glm::vec3& direction)
		{
			const glm::vec3 dir = glm::normalize(direction);
			// Near-vertical aims need a different up or quatLookAt degenerates.
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
