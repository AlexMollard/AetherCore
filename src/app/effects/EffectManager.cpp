#include "EffectManager.hpp"

#include <string>

#include "material/EffectParamBuffer.hpp"
#include "material/PipelineCache.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
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

	bool ApplyEntityEffect(World& world, Entity entity, std::string_view name, const EffectManager& effects, PipelineCache& pipelineCache, EffectParamBuffer& buffer, const EffectParams* overrideParams)
	{
		const std::string nameStr(name);
		const EffectDef* def = effects.Find(nameStr.c_str());
		if (def == nullptr)
		{
			return false;
		}

		const GraphicsPipeline* pipeline = pipelineCache.Acquire(def->templateDesc);
		world.EmplaceOrReplace<PipelineComponent>(entity, PipelineComponent{.pipeline = pipeline});

		// Reuse an existing slot (re-applying an effect) so we neither leak nor
		// double-allocate; otherwise allocate one.
		std::uint32_t slot = EffectParamBuffer::kInvalidSlot;
		if (const auto* existing = world.TryGet<EffectParamsComponent>(entity))
		{
			slot = existing->paramSlot;
		}
		if (slot == EffectParamBuffer::kInvalidSlot)
		{
			slot = buffer.AllocateSlot();
		}

		const EffectParams& params = overrideParams ? *overrideParams : def->defaultParams;
		world.EmplaceOrReplace<EffectParamsComponent>(entity, EffectParamsComponent{slot, params});
		buffer.Write(slot, params);
		world.EmplaceOrReplace<EffectRefComponent>(entity, EffectRefComponent{.name = nameStr});
		return true;
	}
} // namespace aether::app::effects
