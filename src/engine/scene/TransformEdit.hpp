#pragma once

#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	namespace detail
	{
		// Moves `entity`'s children with it from oldWorld to newWorld. Each child is recomposed as
		// newWorld * its local transform rather than multiplied by newWorld * inverse(oldWorld):
		// the delta form feeds that product's float error back into the child every call, and a
		// parent a script sets every tick walked its mesh children over a metre off it in two
		// hours of Play (Crash's model swung round his collider as he turned). The local
		// transform is re-derived only when the parent or the child was changed by anything else.
		inline void MoveChildren(World& world, Entity entity, const glm::mat4& oldWorld, const glm::mat4& newWorld)
		{
			const auto* h = world.TryGet<HierarchyComponent>(entity);
			if (h == nullptr)
			{
				return;
			}
			for (const Entity child: h->children)
			{
				auto* tc = world.TryGet<TransformComponent>(child);
				if (tc == nullptr)
				{
					// A transformless link passes its parent's move straight through.
					MoveChildren(world, child, oldWorld, newWorld);
					continue;
				}
				const glm::mat4 oldChild = tc->localToWorld;
				if (auto* ch = world.TryGet<HierarchyComponent>(child))
				{
					if (ch->cachedParentWorld != oldWorld || ch->cachedWorld != oldChild)
					{
						ch->cachedLocal = glm::inverse(oldWorld) * oldChild;
					}
					tc->localToWorld = newWorld * ch->cachedLocal;
					ch->cachedParentWorld = newWorld;
					ch->cachedWorld = tc->localToWorld;
				}
				MoveChildren(world, child, oldChild, tc->localToWorld);
			}
		}
	} // namespace detail

	inline void SetWorldTransform(World& world, Entity entity, const glm::mat4& localToWorld)
	{
		auto* tc = world.TryGet<TransformComponent>(entity);
		if (tc == nullptr)
		{
			return;
		}
		const glm::mat4 oldWorld = tc->localToWorld;
		tc->localToWorld = localToWorld;
		detail::MoveChildren(world, entity, oldWorld, localToWorld);
	}
} // namespace aether::ecs
