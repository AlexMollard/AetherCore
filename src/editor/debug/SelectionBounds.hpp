#pragma once

#include <optional>
#include <span>

#include <glm/glm.hpp>

#include "mesh/Mesh.hpp"
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

	namespace detail
	{
		// A local-space box through a world matrix. All eight corners, because a rotated box
		// is not axis-aligned any more and transforming only min/max would collapse it.
		inline void GrowByLocalBox(const glm::mat4& localToWorld, glm::vec3 localMin, glm::vec3 localMax, bool& any, glm::vec3& boxMin, glm::vec3& boxMax)
		{
			for (int corner = 0; corner < 8; ++corner)
			{
				const glm::vec3 local((corner & 1) != 0 ? localMax.x : localMin.x,
				        (corner & 2) != 0 ? localMax.y : localMin.y,
				        (corner & 4) != 0 ? localMax.z : localMin.z);
				const glm::vec3 world = glm::vec3(localToWorld * glm::vec4(local, 1.0f));
				boxMin = any ? glm::min(boxMin, world) : world;
				boxMax = any ? glm::max(boxMax, world) : world;
				any = true;
			}
		}
	} // namespace detail

	// Union of the selected entities' boxes, or nullopt when none of them can be framed.
	//
	// Covers the WHOLE selection: framing only the primary leaves the rest off screen, which
	// is the opposite of what the key is for.
	//
	// An entity is measured by what it actually draws - a mesh's own bounds, or a sprite's
	// quad - so a large model no longer frames from inside itself. Entities that draw nothing
	// (lights, empties, and anything whose mesh has not resolved yet) fall back to their
	// transform scale, which is all the size information they carry.
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

			if (const auto* mesh = world.TryGet<MeshComponent>(entity); mesh != nullptr && mesh->mesh != nullptr)
			{
				detail::GrowByLocalBox(transform->localToWorld, mesh->mesh->GetAABBMin(), mesh->mesh->GetAABBMax(), any, boxMin, boxMax);
				continue;
			}

			if (const auto* sprite = world.TryGet<SpriteRendererComponent>(entity); sprite != nullptr)
			{
				const glm::vec2 size = sprite->pixelSize / glm::max(sprite->pixelsPerUnit, 0.001f);
				const glm::vec2 min = -sprite->pivot * size;
				detail::GrowByLocalBox(transform->localToWorld, glm::vec3(min, 0.0f), glm::vec3(min + size, 0.0f), any, boxMin, boxMax);
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
