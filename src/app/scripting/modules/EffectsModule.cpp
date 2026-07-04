#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "assets/AssetManager.hpp"
#include "effects/EffectManager.hpp"
#include "material/MaterialSystem.hpp"
#include "scene/Components.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace
{
	using namespace aether::app::scripting;
	using aether::app::effects::EffectMaterialComponent;

	// set_entity_effect(world, entity_id, "effect_name")
	// Replaces PipelineComponent + MaterialComponent with the named effect's data.
	void das_set_entity_effect(aether::World* w, uint32_t id, const char* name)
	{
		auto& ctx = ActiveContext();
		if (!ctx.effects)
		{
			AE_WARN(aether::LogCategory::App, "set_entity_effect: no EffectManager available");
			return;
		}
		const auto effect = ctx.effects->Find(name);
		if (!effect)
		{
			AE_WARN(aether::LogCategory::App, "set_entity_effect: unknown effect '{}'", name);
			return;
		}

		const aether::Entity e{id};
		w->EmplaceOrReplace<aether::PipelineComponent>(e, aether::PipelineComponent{.pipeline = &effect->pipeline});
		// Keep the authoring data on the entity so the parameter setters below
		// can rebuild + re-acquire (materials are immutable in the registry).
		w->EmplaceOrReplace<EffectMaterialComponent>(e, EffectMaterialComponent{.asset = effect->material});
		aether::MaterialSystem::AssignMaterial(*w, e, ctx.assets->GetMaterialRegistry(), effect->material);
	}

	// -- Effect parameter setters ---------------------------------------------
	// Each mutates the entity's stored effect asset and re-acquires a material
	// (the shader reinterprets the material fields for effect-specific meaning).

	template<typename Mutate>
	void MutateEffectMaterial(aether::World* w, uint32_t id, Mutate mutate)
	{
		const aether::Entity e{id};
		auto* state = w->TryGet<EffectMaterialComponent>(e);
		if (!state)
		{
			return;
		}
		mutate(state->asset);
		aether::MaterialSystem::AssignMaterial(*w, e, ActiveContext().assets->GetMaterialRegistry(), state->asset);
	}

	// set_effect_color(world, entity_id, r, g, b) -> sets emissiveFactor.xyz
	void das_set_effect_color(aether::World* w, uint32_t id, float r, float g, float b)
	{
		MutateEffectMaterial(w, id, [&](aether::MaterialAsset& a) { a.emissiveFactor = glm::vec3(r, g, b); });
	}

	// set_effect_speed(world, entity_id, speed) -> sets metallicFactor
	void das_set_effect_speed(aether::World* w, uint32_t id, float speed)
	{
		MutateEffectMaterial(w, id, [&](aether::MaterialAsset& a) { a.metallicFactor = speed; });
	}

	// set_effect_scale(world, entity_id, scale) -> sets roughnessFactor
	void das_set_effect_scale(aether::World* w, uint32_t id, float scale)
	{
		MutateEffectMaterial(w, id, [&](aether::MaterialAsset& a) { a.roughnessFactor = scale; });
	}

	// set_effect_intensity(world, entity_id, intensity) -> sets occlusionStrength
	void das_set_effect_intensity(aether::World* w, uint32_t id, float intensity)
	{
		MutateEffectMaterial(w, id, [&](aether::MaterialAsset& a) { a.occlusionStrength = intensity; });
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
