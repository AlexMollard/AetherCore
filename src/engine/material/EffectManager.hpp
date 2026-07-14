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
	struct EffectDef
	{
		aether::MaterialTemplate templateDesc{};
		aether::EffectParams defaultParams{};
	};

	class EffectManager
	{
	public:
		EffectManager() = default;
		~EffectManager() = default;

		EffectManager(const EffectManager&) = delete;
		EffectManager& operator=(const EffectManager&) = delete;

		void Register(const char* name, const EffectDef& def);

		[[nodiscard]] const EffectDef* Find(const char* name) const;

		[[nodiscard]] std::size_t Count() const
		{
			return m_effects.size();
		}

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

	bool ApplyEntityEffect(World& world, Entity entity, std::string_view name, const EffectManager& effects, PipelineCache& pipelineCache, EffectParamBuffer& buffer, const EffectParams* overrideParams = nullptr);
} // namespace aether::effects
