#pragma once

#include <algorithm>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
	// True if the entity or any of its ancestors carries SceneTransientComponent.
	// Transient subtrees are script-owned runtime state: scene capture excludes
	// them and replace-all restores spare them (the script respawns/keeps them).
	inline bool HasSceneTransientAncestor(World& world, Entity entity)
	{
		Entity cur = entity;
		while (cur.IsValid())
		{
			if (world.Has<SceneTransientComponent>(cur))
			{
				return true;
			}
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			cur = h ? h->parent : Entity{};
		}
		return false;
	}

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
		if (!child.IsValid() || !world.GetRegistry().valid(World::ToEntt(child)))
		{
			return;
		}
		auto* ch = world.TryGet<HierarchyComponent>(child);
		if (!ch || !ch->parent.IsValid())
		{
			world.RegisterRoot(child);
			return;
		}
		if (auto* ph = world.TryGet<HierarchyComponent>(ch->parent))
		{
			auto& kids = ph->children;
			kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
		}
		ch->parent = {};
		world.RegisterRoot(child);
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

		if (auto* old = world.TryGet<HierarchyComponent>(child); old != nullptr && old->parent.IsValid())
		{
			if (auto* ph = world.TryGet<HierarchyComponent>(old->parent))
			{
				auto& kids = ph->children;
				kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
			}
		}
		else
		{
			world.UnregisterRoot(child);
		}

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
			world.UnregisterRoot(child);
			auto* ph = world.TryGet<HierarchyComponent>(parent);
			if (!ph)
			{
				ph = &world.Emplace<HierarchyComponent>(parent);
			}
			ph->children.push_back(child);
		}
		else
		{
			world.RegisterRoot(child);
		}
		return true;
	}

	// Same as SetParent but inserts `child` at a specific position in the
	// parent's children list. A negative or out-of-range index appends.
	// Returns false on failure (same guards as SetParent).
	inline bool InsertChildAt(World& world, Entity child, Entity parent, int index)
	{
		if (!child.IsValid() || child == parent)
		{
			return false;
		}
		if (parent.IsValid() && IsAncestor(world, parent, child))
		{
			return false;
		}

		if (auto* old = world.TryGet<HierarchyComponent>(child); old != nullptr && old->parent.IsValid())
		{
			if (auto* ph = world.TryGet<HierarchyComponent>(old->parent))
			{
				auto& kids = ph->children;
				kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
			}
		}
		else
		{
			world.UnregisterRoot(child);
		}

		auto* ch = world.TryGet<HierarchyComponent>(child);
		if (!ch)
		{
			ch = &world.Emplace<HierarchyComponent>(child);
		}
		ch->parent = parent;

		if (parent.IsValid())
		{
			world.UnregisterRoot(child);
			auto* ph = world.TryGet<HierarchyComponent>(parent);
			if (!ph)
			{
				ph = &world.Emplace<HierarchyComponent>(parent);
			}
			if (index >= 0 && static_cast<std::size_t>(index) < ph->children.size())
			{
				ph->children.insert(ph->children.begin() + index, child);
			}
			else
			{
				ph->children.push_back(child);
			}
		}
		else
		{
			world.InsertRootAt(child, index);
		}
		return true;
	}

	// Recursively destroys `entity` and its whole subtree, keeping parent links tidy.
	inline void DestroyHierarchy(World& world, Entity entity)
	{
		std::vector<Entity> kids; // copy - the loop mutates the source vector
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
