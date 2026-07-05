#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "material/EffectParams.hpp"
#include "material/MaterialTemplate.hpp"

namespace aether
{
	class EffectParamBuffer;
	class PipelineCache;
	class World;
	struct Entity;
} // namespace aether

namespace aether::effects
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

		// Enumerate registered effect names (unordered) - the editor's
		// add-component palette builds its effect entries from this.
		template<typename Fn>
		void ForEachEffect(Fn&& fn) const
		{
			for (const auto& [name, def]: m_effects)
			{
				fn(name, def);
			}
		}

	private:
		std::unordered_map<std::string, EffectDef> m_effects;
	};

	// Makes `entity` effect-driven by name: resolves the effect pipeline, keeps
	// (or allocates) the per-entity param slot, writes params to the buffer and
	// records EffectRefComponent for scene serialization. `overrideParams`
	// replaces the effect's defaults (scene load restores saved params through
	// it). Shared by the set_entity_effect das binding and the scene loader.
	// Returns false for an unknown effect name.
	bool ApplyEntityEffect(World& world, Entity entity, std::string_view name, const EffectManager& effects, PipelineCache& pipelineCache, EffectParamBuffer& buffer, const EffectParams* overrideParams = nullptr);
} // namespace aether::effects
