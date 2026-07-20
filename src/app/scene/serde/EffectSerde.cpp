// Custom scene serde for the effect components (EffectRef + EffectParams). Apply needs
// the effect manager / pipeline cache / param buffer from the load-time deps, so it
// cannot be a plain reflected field - it lives here instead of in the capture/apply
// loops, registered through the serde table.

#include "scene/SceneComponentSerde.hpp"

#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scene
{
	namespace
	{
		void CaptureEffect(SceneCaptureContext& c)
		{
			const auto* er = c.world.TryGet<EffectRefComponent>(c.entity);
			if (er == nullptr)
			{
				return;
			}
			EffectRecord fx;
			fx.name = er->name;
			if (const auto* ep = c.world.TryGet<EffectParamsComponent>(c.entity))
			{
				fx.params = ep->params;
			}
			c.rec.effect = std::move(fx);
		}

		void ApplyEffect(SceneApplyContext& c)
		{
			if (!c.rec.effect)
			{
				return;
			}
			if (!c.rec.effect->name.empty() && c.deps.effectManager != nullptr && c.deps.effectParams != nullptr && c.deps.pipelines != nullptr)
			{
				if (effects::ApplyEntityEffect(c.world, c.entity, c.rec.effect->name, *c.deps.effectManager, *c.deps.pipelines, *c.deps.effectParams, &c.rec.effect->params))
				{
					++c.effectCount;
				}
				else
				{
					AE_WARN(LogCategory::App, "Scene load: unknown effect '{}'", c.rec.effect->name);
				}
			}
			else
			{
				AE_WARN(LogCategory::App, "Scene load: effect '{}' on '{}' skipped (missing effect deps)", c.rec.effect->name, c.rec.name);
			}
		}

		AE_SCENE_SERDE(Effect, "Effect", 60, CaptureEffect, ApplyEffect)
	} // namespace
} // namespace aether::app::scene
