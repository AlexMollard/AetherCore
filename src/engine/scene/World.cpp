#include "scene/World.hpp"

#include <algorithm>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	entt::entity World::ToEntt(Entity entity) noexcept
	{
		return static_cast<entt::entity>(entity.id);
	}

	Entity World::FromEntt(entt::entity entity) noexcept
	{
		return Entity{static_cast<std::uint32_t>(entt::to_integral(entity))};
	}

	World::World()
	{
		// ecs::SetParent, which then never links the children of a raw-0 parent (the
		const entt::entity nullSlot = m_registry.create();
		m_registry.destroy(nullSlot);
	}

	Entity World::Create()
	{
		const Entity entity = FromEntt(m_registry.create());
		RegisterRoot(entity);
		return entity;
	}

	Entity World::CreateWithId(Entity desired)
	{
		if (!desired.IsValid())
		{
			return Create();
		}
		// entt honours the hint (same index + version) when that index slot is free;
		// otherwise it allocates a fresh identifier, which the caller must remap to.
		const Entity entity = FromEntt(m_registry.create(ToEntt(desired)));
		RegisterRoot(entity);
		return entity;
	}

	void World::Destroy(Entity entity)
	{
		const entt::entity enttEntity = ToEntt(entity);
		if (!entity.IsValid() || !m_registry.valid(enttEntity))
		{
			return;
		}
		// Destroy the subtree, never orphan it: surviving children would hold a
		// parent handle pointing at a destroyed id and silently drop out of the
		// root list, so transform propagation stops at the dead link. Children
		// first - each is detached from its dying parent before its own walk, so
		// even a cyclic hand-authored scene cannot recurse forever - then unlink
		// ourselves from our parent, then destroy.
		std::vector<Entity> kids;
		if (auto* h = m_registry.try_get<HierarchyComponent>(enttEntity))
		{
			kids = h->children;
			h->children.clear();
		}
		for (const Entity child: kids)
		{
			// A malformed scene can list the same child twice, and the first walk
			// already destroyed it - re-check before touching its storage.
			if (!child.IsValid() || !m_registry.valid(ToEntt(child)))
			{
				continue;
			}
			if (auto* ch = m_registry.try_get<HierarchyComponent>(ToEntt(child)))
			{
				ch->parent = {};
			}
			Destroy(child);
		}
		if (!m_registry.valid(enttEntity))
		{
			// A cyclic hierarchy destroyed us from inside the loop above.
			return;
		}
		if (auto* h = m_registry.try_get<HierarchyComponent>(enttEntity); h != nullptr && h->parent.IsValid())
		{
			if (auto* ph = m_registry.try_get<HierarchyComponent>(ToEntt(h->parent)))
			{
				auto& siblings = ph->children;
				siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
			}
		}
		UnregisterRoot(entity);
		m_registry.destroy(enttEntity);
	}

	void World::RegisterRoot(Entity entity)
	{
		const entt::entity enttEntity = ToEntt(entity);
		if (!entity.IsValid() || !m_registry.valid(enttEntity))
		{
			return;
		}
		if (std::find(m_rootOrder.begin(), m_rootOrder.end(), entity) == m_rootOrder.end())
		{
			m_rootOrder.push_back(entity);
		}
	}

	void World::UnregisterRoot(Entity entity)
	{
		m_rootOrder.erase(std::remove(m_rootOrder.begin(), m_rootOrder.end(), entity), m_rootOrder.end());
	}

	void World::InsertRootAt(Entity entity, int index)
	{
		const entt::entity enttEntity = ToEntt(entity);
		if (!entity.IsValid() || !m_registry.valid(enttEntity))
		{
			return;
		}

		UnregisterRoot(entity);
		if (index >= 0 && static_cast<std::size_t>(index) < m_rootOrder.size())
		{
			m_rootOrder.insert(m_rootOrder.begin() + index, entity);
		}
		else
		{
			m_rootOrder.push_back(entity);
		}
	}

	World::~World()
	{
		m_systems.Shutdown(*this);
	}

	void World::RegisterSystem(std::unique_ptr<System> system)
	{
		if (system)
		{
			system->OnRegister(*this);
			m_systems.Register(std::move(system));
		}
	}

	void World::UnregisterSystem(const char* name)
	{
		m_systems.Unregister(name);
	}

	void World::UpdateSystems(float dt)
	{
		AE_PROFILE_ZONE();
		m_systems.UpdateAll(*this, dt);
	}

	System* World::FindSystem(const char* name)
	{
		return m_systems.Find(name);
	}
} // namespace aether
