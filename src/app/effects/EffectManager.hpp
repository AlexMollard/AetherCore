#pragma once

#include <string>
#include <unordered_map>

#include "material/EffectParams.hpp"
#include "material/MaterialTemplate.hpp"

namespace aether::app::effects
{
	// A named effect definition: the pipeline template + default per-entity params.
	// Holds no GPU resources - the pipeline is resolved lazily by PipelineCache and
	// the per-entity params live in EffectParamBuffer.
	struct EffectDef
	{
		aether::MaterialTemplate templateDesc{};
		aether::EffectParams defaultParams{};
	};

	// Application-level registry of named runtime shader effects. Effects are
	// registered once at startup and looked up by name in set_entity_effect.
	class EffectManager
	{
	public:
		EffectManager() = default;
		~EffectManager() = default;

		EffectManager(const EffectManager&) = delete;
		EffectManager& operator=(const EffectManager&) = delete;

		// Register (or replace) a named effect definition.
		void Register(const char* name, const EffectDef& def);

		// Lookup. Returns nullptr if name not found.
		[[nodiscard]] const EffectDef* Find(const char* name) const;

		[[nodiscard]] std::size_t Count() const
		{
			return m_effects.size();
		}

	private:
		std::unordered_map<std::string, EffectDef> m_effects;
	};
} // namespace aether::app::effects
