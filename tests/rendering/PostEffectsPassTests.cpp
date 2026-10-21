#include <doctest/doctest.h>

#include "gpu/PostEffectsPass.hpp"
#include <limits>

TEST_CASE("Portable post stack preserves native ordering and neutral settings")
{
	using namespace aether::gpu;
	PostEffectsSettings settings;
	auto neutral = PlanPostEffects(settings, 512, 256, false);
	REQUIRE(neutral);
	CHECK(neutral->width == 512);
	CHECK_FALSE(neutral->auxiliary);
	CHECK(neutral->stageCount == 3);
	CHECK(neutral->stages[0] == PostEffectStage::Forward);
	CHECK(neutral->stages[1] == PostEffectStage::Tonemap);
	CHECK(neutral->stages[2] == PostEffectStage::DisplayUi);
	settings.gtao = settings.reflections = settings.volumetrics = settings.fxaa = true;
	settings.bloomStrength = .1f;
	auto full = PlanPostEffects(settings, 512, 256, true);
	REQUIRE(full);
	CHECK(full->auxiliary);
	const auto index = [&](PostEffectStage stage)
	{
		for (std::uint32_t i = 0; i < full->stageCount; ++i)
		{
			if (full->stages[i] == stage) { return i; }
		}
		return UINT32_MAX;
	};
	CHECK(index(PostEffectStage::OpaquePrepass) < index(PostEffectStage::Gtao));
	CHECK(index(PostEffectStage::GtaoDenoise) < index(PostEffectStage::Forward));
	CHECK(index(PostEffectStage::Forward) < index(PostEffectStage::Reflection));
	CHECK(index(PostEffectStage::ReflectionComposite) < index(PostEffectStage::Volumetric));
	CHECK(index(PostEffectStage::DepthOfField) < index(PostEffectStage::BloomDown));
	CHECK(index(PostEffectStage::BloomUp) < index(PostEffectStage::Tonemap));
	CHECK(index(PostEffectStage::Tonemap) < index(PostEffectStage::Fxaa));
	CHECK(index(PostEffectStage::Fxaa) < index(PostEffectStage::DisplayUi));
}

TEST_CASE("Portable post stack rejects invalid settings and bounds allocation")
{
	using namespace aether::gpu;
	PostEffectsSettings settings;
	CHECK_FALSE(PlanPostEffects(settings, 0, 256, false));
	CHECK_FALSE(PlanPostEffects(settings, 4097, 256, false));
	settings.filmGrain = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(PlanPostEffects(settings, 512, 256, false));
	settings.filmGrain = 0;
	settings.renderScale = .5f;
	auto scaled = PlanPostEffects(settings, 512, 256, false);
	REQUIRE(scaled);
	CHECK(scaled->width == 256);
	CHECK(scaled->height == 128);
	const auto small = PlanPostEffects(settings, 64, 63, false);
	REQUIRE(small);
	CHECK(small->width == 64);
	CHECK(small->height == 64);
	settings.renderScale = 2;
	settings.gtao = settings.reflections = settings.fxaa = true;
	CHECK_FALSE(PlanPostEffects(settings, 4096, 4096, true));
}
