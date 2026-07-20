#include <algorithm>

#include "scene/System.hpp"

#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	bool System::IsActiveIn(const World& world) const
	{
		return HasAllSceneFeatures(world.GetSceneFeatures(), RequiredFeatures());
	}

	void SystemRegistry::Register(std::unique_ptr<System> system)
	{
		AE_PROFILE_ZONE();
		if (!system)
		{
			return;
		}

		AE_VERBOSE(LogCategory::Engine, "Registering system: {}", system->GetName());
		m_systems.push_back(std::move(system));
	}

	void SystemRegistry::Unregister(const char* name)
	{
		AE_PROFILE_ZONE();
		auto it = std::ranges::find_if(m_systems, [name](const std::unique_ptr<System>& sys) { return sys && std::string_view(sys->GetName()) == name; });
		if (it != m_systems.end())
		{
			AE_VERBOSE(LogCategory::Engine, "Unregistering system: {}", name);
			m_systems.erase(it);
		}
	}

	void SystemRegistry::UpdateAll(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		for (auto& system: m_systems)
		{
			if (system && system->IsActiveIn(world))
			{
				AE_PROFILE_ZONE_N("SystemRegistry.UpdateSystem");
				AE_PROFILE_SET_ZONE_NAME(system->GetName());
				system->Update(world, dt);
			}
		}
	}

	void SystemRegistry::Shutdown(World& world)
	{
		AE_PROFILE_ZONE();
		for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it)
		{
			if (*it)
			{
				AE_VERBOSE(LogCategory::Engine, "Shutting down system: {}", (*it)->GetName());
				(*it)->OnUnregister(world);
			}
		}
		m_systems.clear();
	}

	System* SystemRegistry::Find(const char* name)
	{
		auto it = std::ranges::find_if(m_systems, [name](const std::unique_ptr<System>& sys) { return sys && std::string_view(sys->GetName()) == name; });
		return it != m_systems.end() ? it->get() : nullptr;
	}
} // namespace aether
