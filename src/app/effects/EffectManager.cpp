#include "EffectManager.hpp"

#include "utils/Profiler.hpp"

namespace aether::app::effects
{
	void EffectManager::Register(const char* name, const EffectDef& def)
	{
		AE_PROFILE_ZONE();
		m_effects[name ? name : ""] = def;
	}

	const EffectDef* EffectManager::Find(const char* name) const
	{
		auto it = m_effects.find(name ? name : "");
		return it != m_effects.end() ? &it->second : nullptr;
	}
} // namespace aether::app::effects
