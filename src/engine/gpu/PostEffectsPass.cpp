#include "PostEffectsPass.hpp"

#include <algorithm>

namespace aether::gpu
{
	std::expected<PostEffectsPlan, std::string> PlanPostEffects(const PostEffectsSettings& settings, std::uint32_t width, std::uint32_t height, bool depthOfField)
	{
		if (!width || !height || width > 4096 || height > 4096)
		{
			return std::unexpected("WEB_EFFECTS_EXTENT: output extent must be within 1..4096");
		}
		for (float value: {settings.gradeContrast, settings.gradeSaturation, settings.gradeTemperature, settings.gradeTint,
		             settings.vignetteIntensity, settings.vignetteRoundness, settings.chromaticAberration, settings.sharpness, settings.filmGrain,
		             settings.motionBlur, settings.motionBlurMaxRadius, settings.gtaoRadius, settings.gtaoStrength, settings.specularFilter,
		             settings.foliageSkyLook, settings.bloomStrength, settings.bloomRadius, settings.exposureKey, settings.exposureSpeed,
		             settings.reflectionMaxRoughness, settings.reflectionIntensity, settings.renderScale})
		{
			if (!std::isfinite(value))
			{
				return std::unexpected("WEB_EFFECTS_NONFINITE: all effect settings must be finite");
			}
		}
		if (settings.renderScale < .25f || settings.renderScale > 2 || settings.gtaoRadius <= 0 || settings.gtaoStrength < 0
		        || settings.motionBlur < 0 || settings.motionBlurMaxRadius < 0 || settings.exposureKey <= 0 || settings.exposureSpeed < 0)
		{
			return std::unexpected("WEB_EFFECTS_SETTINGS: invalid render scale, radius, exposure or motion settings");
		}
		PostEffectsPlan plan;
		plan.width = std::max(1u, static_cast<std::uint32_t>(width * settings.renderScale));
		plan.height = std::max(1u, static_cast<std::uint32_t>(height * settings.renderScale));
		if (plan.width > 4096 || plan.height > 4096)
		{
			return std::unexpected("WEB_EFFECTS_EXTENT: scaled scene exceeds the 4096-pixel limit");
		}
		plan.auxiliary = settings.gtao || settings.reflections || settings.volumetrics || depthOfField || settings.motionBlur > 0;
		plan.depthOfField = depthOfField;
		const auto pixels = std::uint64_t(plan.width) * plan.height;
		// HDR, opaque targets, depth and LDR; optional half-res buffers and five bloom mips.
		plan.estimatedBytes = pixels * 28 + std::uint64_t(width) * height * 4;
		if (settings.gtao) { plan.estimatedBytes += pixels; }
		if (settings.reflections) { plan.estimatedBytes += pixels * 8; }
		if (settings.volumetrics || depthOfField) { plan.estimatedBytes += pixels * 2 * (settings.volumetrics + depthOfField); }
		if (settings.bloomStrength > 0) { plan.estimatedBytes += pixels * 8 / 3; }
		if (plan.estimatedBytes > 512ull * 1024 * 1024)
		{
			return std::unexpected("WEB_EFFECTS_BUDGET: transient effects exceed 512 MiB; lower renderScale or disable effects");
		}
		const auto add = [&](PostEffectStage stage) { plan.stages[plan.stageCount++] = stage; };
		if (plan.auxiliary) { add(PostEffectStage::OpaquePrepass); }
		if (settings.gtao) { add(PostEffectStage::Gtao); add(PostEffectStage::GtaoDenoise); }
		add(PostEffectStage::Forward);
		if (settings.reflections) { add(PostEffectStage::Reflection); add(PostEffectStage::ReflectionComposite); }
		if (settings.volumetrics) { add(PostEffectStage::Volumetric); add(PostEffectStage::VolumetricComposite); }
		if (depthOfField) { add(PostEffectStage::DepthOfField); }
		if (settings.bloomStrength > 0) { add(PostEffectStage::BloomDown); add(PostEffectStage::BloomUp); }
		add(PostEffectStage::Tonemap);
		if (settings.fxaa || settings.sharpness > 0 || settings.renderScale != 1) { add(PostEffectStage::Fxaa); }
		add(PostEffectStage::DisplayUi);
		return plan;
	}
}
