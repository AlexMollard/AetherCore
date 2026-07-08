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

	// -- Entity lifecycle ------------------------------------------------------

	World::World()
	{
		// entt hands out entity index 0 / version 0 as the very first entity in a
		// fresh registry. Its full integral value is 0 - precisely the value
		// aether::Entity reserves for the null / invalid entity (Entity{}.IsValid()
		// == false, ToEntt/FromEntt keep the whole index+version integral). Left
		// untouched, the first real Create() would return a live entity that reports
		// IsValid() == false, silently breaking every IsValid()-gated path - e.g.
		// ecs::SetParent, which then never links the children of a raw-0 parent (the
		// bug that kept a scene's first-loaded UI canvas empty and rendered nothing).
		//
		// Retire that null slot once, here: create index 0 (version 0) and destroy
		// it immediately. entt bumps index 0's version to 1, so the next Create()
		// recycles it as (index 0, version 1) whose integral is (1u << 20) != 0 =>
		// IsValid() == true. The destroy leaves the registry with zero live entities,
		// so there is NO phantom entity: component iteration, entity counts and
		// CaptureScene (which already filters !reg.valid handles) are all untouched,
		// and raw value 0 stays permanently reserved for the null entity.
		const entt::entity nullSlot = m_registry.create();
		m_registry.destroy(nullSlot);
	}

	Entity World::Create()
	{
		const Entity entity = FromEntt(m_registry.create());
		RegisterRoot(entity);
		return entity;
	}

	void World::Destroy(Entity entity)
	{
		const entt::entity enttEntity = ToEntt(entity);
		if (entity.IsValid() && m_registry.valid(enttEntity))
		{
			UnregisterRoot(entity);
			m_registry.destroy(enttEntity);
		}
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
