#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <string>

namespace aether::gpu
{
	// Neutral defaults preserve explicitly unlit development/reference scenes.
	// Authored graphics settings populate this owned snapshot on the game thread.
	struct PostEffectsSettings
	{
		bool fxaa = false, gtao = false, autoExposure = false, reflections = false, volumetrics = false;
		float gradeContrast = 1, gradeSaturation = 1, gradeTemperature = 0, gradeTint = 0;
		float vignetteIntensity = 0, vignetteRoundness = 1, chromaticAberration = 0, sharpness = 0, filmGrain = 0;
		float motionBlur = 0, motionBlurMaxRadius = 64;
		float gtaoRadius = 1.4f, gtaoStrength = 1.35f, specularFilter = 1, foliageSkyLook = 1;
		float bloomStrength = 0, bloomRadius = 1;
		float exposureKey = .18f, exposureSpeed = 2;
		float reflectionMaxRoughness = .45f, reflectionIntensity = 1;
		float renderScale = 1;
	};

	enum class PostEffectStage : std::uint8_t
	{
		OpaquePrepass, Gtao, GtaoDenoise, Forward, Reflection, ReflectionComposite,
		Volumetric, VolumetricComposite, DepthOfField, BloomDown, BloomUp, Tonemap, Fxaa, DisplayUi
	};

	struct PostEffectsPlan
	{
		std::uint32_t width = 0, height = 0;
		std::uint64_t estimatedBytes = 0;
		bool auxiliary = false, depthOfField = false;
		std::array<PostEffectStage, 32> stages{};
		std::uint32_t stageCount = 0;
	};

	[[nodiscard]] std::expected<PostEffectsPlan, std::string> PlanPostEffects(const PostEffectsSettings& settings, std::uint32_t width, std::uint32_t height, bool depthOfField);
}
