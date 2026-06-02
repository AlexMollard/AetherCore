#include "scene/World.hpp"

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

	// ── Entity lifecycle ──────────────────────────────────────────────────────

	Entity World::Create()
	{
		return FromEntt(m_registry.create());
	}

	void World::Destroy(Entity entity)
	{
		const entt::entity enttEntity = ToEntt(entity);
		if (entity.IsValid() && m_registry.valid(enttEntity))
		{
			m_registry.destroy(enttEntity);
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
} // namespace aether
