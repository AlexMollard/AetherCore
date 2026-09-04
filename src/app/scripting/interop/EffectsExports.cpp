#include "scripting/interop/InteropCommon.hpp"

#include "assets/AssetManager.hpp"
#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	template<typename Mutate>
	void MutateEffectParams(std::uint32_t id, Mutate mutate)
	{
		auto* comp = ActiveWorld().TryGet<aether::EffectParamsComponent>(aether::Entity{id});
		if (comp == nullptr)
		{
			return;
		}
		mutate(comp->params);
		ActiveContext().assets->GetEffectParamBuffer().Write(comp->paramSlot, comp->params);
	}
} // namespace

AE_SCRIPT_API void aether_effect_set(std::uint32_t id, const char* name)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.effects == nullptr || ctx.assets == nullptr)
	{
		AE_WARN(aether::LogCategory::App, "set_effect: no EffectManager/assets");
		return;
	}
	if (!aether::effects::ApplyEntityEffect(ActiveWorld(), aether::Entity{id}, name != nullptr ? name : "", *ctx.effects, ctx.assets->GetPipelineCache(), ctx.assets->GetEffectParamBuffer()))
	{
		AE_WARN(aether::LogCategory::App, "set_effect: unknown effect '{}'", name != nullptr ? name : "");
	}
	});
}

AE_SCRIPT_API void aether_effect_set_color(std::uint32_t id, Vec3 color)
{ SafeExport([&] -> void { MutateEffectParams(id, [&](aether::EffectParams& p) { p.tint = glm::vec4(ToGlm(color), p.tint.w); }); }); }

AE_SCRIPT_API void aether_effect_set_speed(std::uint32_t id, float speed)
{ SafeExport([&] -> void { MutateEffectParams(id, [&](aether::EffectParams& p) { p.speed = speed; }); }); }

AE_SCRIPT_API void aether_effect_set_scale(std::uint32_t id, float scale)
{ SafeExport([&] -> void { MutateEffectParams(id, [&](aether::EffectParams& p) { p.scale = scale; }); }); }

AE_SCRIPT_API void aether_effect_set_intensity(std::uint32_t id, float intensity)
{ SafeExport([&] -> void { MutateEffectParams(id, [&](aether::EffectParams& p) { p.intensity = intensity; }); }); }
