#pragma once

#include <algorithm>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ecs
{
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

	inline bool HasDisabledAncestor(const World& world, Entity entity)
	{
		Entity cur = entity;
		while (cur.IsValid())
		{
			if (world.Has<DisabledComponent>(cur))
			{
				return true;
			}
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			cur = h ? h->parent : Entity{};
		}
		return false;
	}

	inline bool IsActiveInHierarchy(const World& world, Entity entity)
	{
		return !HasDisabledAncestor(world, entity);
	}

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

	inline bool SetParent(World& world, Entity child, Entity parent)
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
			ph->children.push_back(child);
		}
		else
		{
			world.RegisterRoot(child);
		}
		return true;
	}

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

	inline void DestroyHierarchy(World& world, Entity entity)
	{
		std::vector<Entity> kids;
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
