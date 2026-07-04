#pragma once

#include <algorithm>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	// True if `possibleAncestor` is `entity` itself or any ancestor of it.
	inline bool IsAncestor(World& world, Entity entity, Entity possibleAncestor)
	{
		Entity cur = entity;
		while (cur.IsValid())
		{
			if (cur == possibleAncestor)
			{
				return true;
			}
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			if (!h)
			{
				break;
			}
			cur = h->parent;
		}
		return false;
	}

	// Removes `child` from its current parent's child list and clears its parent.
	inline void DetachFromParent(World& world, Entity child)
	{
		auto* ch = world.TryGet<HierarchyComponent>(child);
		if (!ch || !ch->parent.IsValid())
		{
			return;
		}
		if (auto* ph = world.TryGet<HierarchyComponent>(ch->parent))
		{
			auto& kids = ph->children;
			kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
		}
		ch->parent = {};
	}

	// Re-parents `child` under `parent` (parent == {0} detaches to root).
	// Returns false (no-op) if child == parent or it would create a cycle.
	inline bool SetParent(World& world, Entity child, Entity parent)
	{
		if (!child.IsValid() || child == parent)
		{
			return false;
		}
		if (parent.IsValid() && IsAncestor(world, parent, child))
		{
			return false; // cycle: parent is a descendant of child
		}

		DetachFromParent(world, child);

		// Set the child side first; emplacing on the parent below may reallocate
		// the HierarchyComponent pool and invalidate this pointer.
		auto* ch = world.TryGet<HierarchyComponent>(child);
		if (!ch)
		{
			ch = &world.Emplace<HierarchyComponent>(child);
		}
		ch->parent = parent;

		if (parent.IsValid())
		{
			auto* ph = world.TryGet<HierarchyComponent>(parent);
			if (!ph)
			{
				ph = &world.Emplace<HierarchyComponent>(parent);
			}
			ph->children.push_back(child);
		}
		return true;
	}

	// Recursively destroys `entity` and its whole subtree, keeping parent links tidy.
	inline void DestroyHierarchy(World& world, Entity entity)
	{
		std::vector<Entity> kids; // copy — the loop mutates the source vector
		if (const auto* h = world.TryGet<HierarchyComponent>(entity))
		{
			kids = h->children;
		}
		for (const Entity c: kids)
		{
			DestroyHierarchy(world, c);
		}
		DetachFromParent(world, entity);
		world.Destroy(entity);
	}
} // namespace aether::ecs
