#include <doctest/doctest.h>

#include "animation/AnimationSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

#include <algorithm>

using namespace aether;

namespace
{
	constexpr float kClipLength = 2.0f;

	SkinnedMeshComponent Playing(std::uint32_t clip, float time)
	{
		SkinnedMeshComponent smc;
		smc.clipIndex = clip;
		smc.animTime = time;
		return smc;
	}
} // namespace

TEST_CASE("A crossfade starts the new clip at zero while the old one keeps its own clock")
{
	SkinnedMeshComponent smc = Playing(3, 0.7f);
	CrossFadeTo(smc, 5, 0.2f);
	CHECK(smc.clipIndex == 5);
	CHECK(smc.animTime == doctest::Approx(0.0f));
	CHECK(smc.fadeClipIndex == 3);
	CHECK(smc.fadeWeight == doctest::Approx(1.0f));

	AdvanceAnimation(smc, 0.1f, kClipLength, kClipLength);
	CHECK(smc.animTime == doctest::Approx(0.1f));
	CHECK(smc.fadeTime == doctest::Approx(0.8f));
	CHECK(smc.fadeWeight == doctest::Approx(0.5f));

	AdvanceAnimation(smc, 0.15f, kClipLength, kClipLength);
	CHECK(smc.fadeWeight == 0.0f);
	CHECK(smc.clipIndex == 5);
	CHECK(smc.animTime == doctest::Approx(0.25f));
}

TEST_CASE("A zero-length crossfade switches outright and cancels a fade in progress")
{
	SkinnedMeshComponent smc = Playing(1, 0.4f);
	CrossFadeTo(smc, 2, 0.5f);
	CrossFadeTo(smc, 0, 0.0f);
	CHECK(smc.clipIndex == 0);
	CHECK(smc.animTime == 0.0f);
	CHECK(smc.fadeWeight == 0.0f);
}

TEST_CASE("Retargeting mid-fade fades out whichever clip shows more")
{
	SkinnedMeshComponent smc = Playing(1, 0.4f);
	CrossFadeTo(smc, 2, 1.0f);
	AdvanceAnimation(smc, 0.2f, kClipLength, kClipLength); // clip 1 still at 0.8
	CrossFadeTo(smc, 3, 1.0f);
	CHECK(smc.fadeClipIndex == 1);
	CHECK(smc.fadeTime == doctest::Approx(0.6f));

	AdvanceAnimation(smc, 0.7f, kClipLength, kClipLength); // clip 3 now dominant
	CrossFadeTo(smc, 4, 1.0f);
	CHECK(smc.fadeClipIndex == 3);
	CHECK(smc.fadeTime == doctest::Approx(0.7f));
	CHECK(smc.fadeWeight == doctest::Approx(1.0f));
}

TEST_CASE("Each clip in a crossfade wraps on its own length and looping flag")
{
	SkinnedMeshComponent smc = Playing(1, 1.9f);
	smc.looping = true;
	CrossFadeTo(smc, 2, 1.0f);
	smc.looping = false;
	AdvanceAnimation(smc, 0.3f, 0.25f, kClipLength);
	CHECK(smc.fadeTime == doctest::Approx(0.2f)); // old clip looped past its 2 s end
	CHECK(smc.animTime == doctest::Approx(0.3f)); // new clip holds past its 0.25 s end
}

TEST_CASE("Gameplay-selected skeletal clocks advance in the current frame at variable frame rates")
{
	class AnimationAuthor final : public System
	{
	public:
		AnimationAuthor(Entity entity, const float& realDelta) : m_entity(entity), m_real_delta(realDelta)
		{
		}
		void Update(World& world, float dt) override
		{
			auto& skin = world.Get<SkinnedMeshComponent>(m_entity);
			skin.playbackSpeed = m_real_delta / dt;
			if (m_first)
			{
				CrossFadeTo(skin, 1, .18f / skin.playbackSpeed);
				m_first = false;
			}
		}
		const char* GetName() const override
		{
			return "AnimationAuthor";
		}
	private:
		Entity m_entity;
		const float& m_real_delta;
		bool m_first = true;
	};
	World world;
	const auto entity = world.Create();
	world.Emplace<SkinnedMeshComponent>(entity, Playing(0, .7f));
	float realDelta = 0;
	world.RegisterSystem(std::make_unique<AnimationAuthor>(entity, realDelta));
	auto animation = std::make_unique<AnimationSystem>();
	animation->SetClipDurationSource([](World&, Entity, const SkinnedMeshComponent&, float& clip, float& fade)
	{
		clip = fade = 100.f;
		return true;
	});
	world.RegisterSystem(std::move(animation));
	float elapsed = 0;
	for (int frame = 0; frame < 40; ++frame)
	{
		realDelta = frame % 2 == 0 ? .2f : 1.f / 60;
		world.UpdateSystems(std::min(realDelta, 1.f / 30));
		elapsed += realDelta;
		const auto& skin = world.Get<SkinnedMeshComponent>(entity);
		CHECK(skin.animTime == doctest::Approx(elapsed).epsilon(.0001));
		CHECK(skin.fadeWeight == 0.f);
	}
}
