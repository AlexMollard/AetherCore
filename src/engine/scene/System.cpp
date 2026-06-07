#include "scene/System.hpp"

#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void SystemRegistry::Register(std::unique_ptr<System> system)
	{
		AE_PROFILE_ZONE();
		if (!system)
		{
			return;
		}

		AE_VERBOSE(LogCategory::Engine, "Registering system: {}", system->GetName());
		m_systems.push_back(std::move(system));
		// Note: OnRegister is called when the system is added to World via
		// RegisterSystem()
	}

	void SystemRegistry::Unregister(const char* name)
	{
		AE_PROFILE_ZONE();
		auto it = std::find_if(m_systems.begin(), m_systems.end(), [name](const std::unique_ptr<System>& sys) { return sys && std::string_view(sys->GetName()) == name; });
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
			if (system)
			{
				system->Update(world, dt);
			}
		}
	}

	void SystemRegistry::Clear()
	{
		m_systems.clear();
	}

	System* SystemRegistry::Find(const char* name)
	{
		auto it = std::find_if(m_systems.begin(), m_systems.end(), [name](const std::unique_ptr<System>& sys) { return sys && std::string_view(sys->GetName()) == name; });
		return it != m_systems.end() ? it->get() : nullptr;
	}
} // namespace aether
