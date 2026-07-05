#pragma once

#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	namespace detail
	{
		inline void ApplyDeltaToSubtree(World& world, Entity entity, const glm::mat4& delta)
		{
			const auto* h = world.TryGet<HierarchyComponent>(entity);
			if (h == nullptr)
			{
				return;
			}
			for (const Entity child: h->children)
			{
				if (auto* tc = world.TryGet<TransformComponent>(child))
				{
					tc->localToWorld = delta * tc->localToWorld;
				}
				ApplyDeltaToSubtree(world, child, delta);
			}
		}
	} // namespace detail

	// Sets the entity's world matrix and applies the same world-space delta to
	// its whole subtree, so children keep their RELATIVE offsets (grandchildren
	// included). This is the editor semantics for moving a parent - the old
	// copy-the-parent-matrix-verbatim rule collapsed every child onto the
	// parent. Returns the delta that was applied.
	inline glm::mat4 SetWorldTransform(World& world, Entity entity, const glm::mat4& localToWorld)
	{
		auto* tc = world.TryGet<TransformComponent>(entity);
		if (tc == nullptr)
		{
			return glm::mat4(1.0f);
		}
		const glm::mat4 delta = localToWorld * glm::inverse(tc->localToWorld);
		tc->localToWorld = localToWorld;
		detail::ApplyDeltaToSubtree(world, entity, delta);
		return delta;
	}
} // namespace aether::ecs
