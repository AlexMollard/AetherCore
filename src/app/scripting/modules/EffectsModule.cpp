#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "assets/AssetManager.hpp"
#include "effects/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/PipelineCache.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "scene/Components.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace
{
	using namespace aether::app::scripting;

	// set_entity_effect(world, entity_id, "effect_name")
	// Resolves the effect pipeline via PipelineCache, allocates a per-entity
	// EffectParams slot, and writes the effect's default params. The presence of
	// EffectParamsComponent marks the entity effect-driven (so a later set_material
	// repaints colour without dropping the effect pipeline).
	void das_set_entity_effect(aether::World* w, uint32_t id, const char* name)
	{
		auto& ctx = ActiveContext();
		if (!ctx.effects || !ctx.assets)
		{
			AE_WARN(aether::LogCategory::App, "set_entity_effect: no EffectManager/assets");
			return;
		}
		const auto* def = ctx.effects->Find(name);
		if (!def)
		{
			AE_WARN(aether::LogCategory::App, "set_entity_effect: unknown effect '{}'", name);
			return;
		}

		const aether::Entity e{id};
		auto& buffer = ctx.assets->GetEffectParamBuffer();

		const aether::GraphicsPipeline* pipeline = ctx.assets->GetPipelineCache().Acquire(def->templateDesc);
		w->EmplaceOrReplace<aether::PipelineComponent>(e, aether::PipelineComponent{.pipeline = pipeline});

		// Reuse an existing slot (re-applying an effect) so we neither leak nor
		// double-allocate; otherwise allocate one.
		std::uint32_t slot = aether::EffectParamBuffer::kInvalidSlot;
		if (const auto* existing = w->TryGet<aether::EffectParamsComponent>(e))
		{
			slot = existing->paramSlot;
		}
		if (slot == aether::EffectParamBuffer::kInvalidSlot)
		{
			slot = buffer.AllocateSlot();
		}
		w->EmplaceOrReplace<aether::EffectParamsComponent>(e, aether::EffectParamsComponent{slot, def->defaultParams});
		buffer.Write(slot, def->defaultParams);
	}

	// -- Effect parameter setters ---------------------------------------------
	// One EffectParamBuffer::Write per call after a CPU-side read-modify-write of
	// the entity's EffectParams (no material re-acquire, no slot churn). No-op on
	// entities without an active effect.
	template<typename Mutate>
	void MutateEffectParams(aether::World* w, uint32_t id, Mutate mutate)
	{
		const aether::Entity e{id};
		auto* comp = w->TryGet<aether::EffectParamsComponent>(e);
		if (!comp)
		{
			return;
		}
		mutate(comp->params);
		ActiveContext().assets->GetEffectParamBuffer().Write(comp->paramSlot, comp->params);
	}

	// set_effect_color(world, entity_id, r, g, b) -> tint.rgb
	void das_set_effect_color(aether::World* w, uint32_t id, float r, float g, float b)
	{
		MutateEffectParams(w, id, [&](aether::EffectParams& p) { p.tint = glm::vec4(r, g, b, p.tint.w); });
	}

	// set_effect_speed(world, entity_id, speed) -> speed
	void das_set_effect_speed(aether::World* w, uint32_t id, float speed)
	{
		MutateEffectParams(w, id, [&](aether::EffectParams& p) { p.speed = speed; });
	}

	// set_effect_scale(world, entity_id, scale) -> scale
	void das_set_effect_scale(aether::World* w, uint32_t id, float scale)
	{
		MutateEffectParams(w, id, [&](aether::EffectParams& p) { p.scale = scale; });
	}

	// set_effect_intensity(world, entity_id, intensity) -> intensity
	void das_set_effect_intensity(aether::World* w, uint32_t id, float intensity)
	{
		MutateEffectParams(w, id, [&](aether::EffectParams& p) { p.intensity = intensity; });
	}
} // namespace

namespace aether::app::scripting
{
	struct EffectsModule : DasModuleBase
	{
		EffectsModule()
		      : DasModuleBase("effects")
		{
			das::ModuleLibrary lib(this);

			Bind<das_set_entity_effect>(lib, "set_entity_effect", SE::modifyExternal);
			Bind<das_set_effect_color>(lib, "set_effect_color", SE::modifyExternal);
			Bind<das_set_effect_speed>(lib, "set_effect_speed", SE::modifyExternal);
			Bind<das_set_effect_scale>(lib, "set_effect_scale", SE::modifyExternal);
			Bind<das_set_effect_intensity>(lib, "set_effect_intensity", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(EffectsModule, aether::app::scripting)
