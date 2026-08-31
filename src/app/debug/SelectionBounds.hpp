#pragma once

#include <optional>
#include <span>

#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	// The box the viewport frames when the user presses F.
	struct FocusBox
	{
		glm::vec3 center{0.0f};
		glm::vec3 size{0.0f};
	};

	// Union of the selected entities' boxes, or nullopt when none of them can be framed.
	//
	// Covers the WHOLE selection: framing only the primary leaves the rest off screen, which
	// is the opposite of what the key is for. An entity's extent is taken from its transform
	// scale - its real bounds live in a mesh or sprite, and plenty of framable entities
	// (lights, empties) have neither - so a lone entity yields exactly the box it always did.
	[[nodiscard]] inline std::optional<FocusBox> ComputeSelectionFocusBox(const World& world, std::span<const Entity> entities)
	{
		glm::vec3 boxMin(0.0f);
		glm::vec3 boxMax(0.0f);
		bool any = false;
		for (const Entity entity: entities)
		{
			if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
			{
				continue;
			}
			const auto* transform = world.TryGet<TransformComponent>(entity);
			if (transform == nullptr)
			{
				continue;
			}
			const glm::vec3 position = glm::vec3(transform->localToWorld[3]);
			const glm::vec3 extent(glm::length(glm::vec3(transform->localToWorld[0])),
			        glm::length(glm::vec3(transform->localToWorld[1])),
			        glm::length(glm::vec3(transform->localToWorld[2])));
			const glm::vec3 half = extent * 0.5f;
			boxMin = any ? glm::min(boxMin, position - half) : position - half;
			boxMax = any ? glm::max(boxMax, position + half) : position + half;
			any = true;
		}
		if (!any)
		{
			return std::nullopt;
		}
		return FocusBox{.center = (boxMin + boxMax) * 0.5f, .size = boxMax - boxMin};
	}
} // namespace aether::editor
