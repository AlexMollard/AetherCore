#pragma once

#include <array>
#include <cmath>
#include <limits>
#include "scene/GrassBenderComponent.hpp"
#include "scene/World.hpp"
#include "scene/Hierarchy.hpp"

namespace aether
{
	// Called on the game thread; the returned frame owns its immutable GPU data.
	inline GrassBenderFrame ExtractGrassBenders(const World& world, glm::vec3 camera)
	{
		GrassBenderFrame result;
		std::array<float, 8> distances;
		std::array<std::uint32_t, 8> ids;
		distances.fill(std::numeric_limits<float>::infinity());
		ids.fill(std::numeric_limits<std::uint32_t>::max());
		auto view = world.GetRegistry().view<const GrassBenderComponent, const TransformComponent>();
		for (const auto entity: view)
		{
			const auto& bender = view.get<const GrassBenderComponent>(entity);
			if (!std::isfinite(bender.radius) || bender.radius <= 0.0f || !std::isfinite(bender.strength) || ecs::HasDisabledAncestor(world, World::FromEntt(entity)))
			{
				continue;
			}
			const glm::vec3 position(view.get<const TransformComponent>(entity).localToWorld[3]);
			const glm::vec3 delta = position - camera;
			const float distance = glm::dot(delta, delta);
			if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) || !std::isfinite(distance)) continue;
			const auto id = static_cast<std::uint32_t>(entity);
			std::size_t index = 0;
			while (index < distances.size() && (distances[index] < distance || (distances[index] == distance && ids[index] < id)))
			{
				++index;
			}
			if (index == distances.size())
			{
				continue;
			}
			for (std::size_t i = distances.size() - 1; i > index; --i)
			{
				distances[i] = distances[i - 1];
				ids[i] = ids[i - 1];
				result.positions[i] = result.positions[i - 1];
				result.parameters[i] = result.parameters[i - 1];
			}
			distances[index] = distance;
			ids[index] = id;
			result.positions[index] = glm::vec4(position, bender.radius);
			result.parameters[index] = glm::vec4(bender.strength, 0.0f, 0.0f, 0.0f);
		}
		return result;
	}
}
