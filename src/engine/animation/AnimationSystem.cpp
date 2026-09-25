#include "animation/AnimationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		float Advance(float time, float dt, float speed, bool looping, float duration)
		{
			time += dt * speed;
			if (looping && duration > 0.f && time > duration)
			{
				time = std::fmod(time, duration);
			}
			return time;
		}
	} // namespace

	void CrossFadeTo(SkinnedMeshComponent& smc, std::uint32_t clip, float seconds)
	{
		if (seconds > 0.f)
		{
			// Mid-fade, keep whichever clip shows more as the one fading out.
			if (smc.fadeWeight <= 0.5f)
			{
				smc.fadeClipIndex = smc.clipIndex;
				smc.fadeTime = smc.animTime;
				smc.fadeLooping = smc.looping;
			}
			smc.fadeWeight = 1.f;
			smc.fadeRate = 1.f / seconds;
		}
		else
		{
			smc.fadeWeight = 0.f;
		}
		smc.clipIndex = clip;
		smc.animTime = 0.f;
	}

	void AdvanceAnimation(SkinnedMeshComponent& smc, float dt, float clipDuration, float fadeClipDuration)
	{
		smc.animTime = Advance(smc.animTime, dt, smc.playbackSpeed, smc.looping, clipDuration);
		if (smc.fadeWeight > 0.f)
		{
			smc.fadeTime = Advance(smc.fadeTime, dt, smc.playbackSpeed, smc.fadeLooping, fadeClipDuration);
			smc.fadeWeight = std::max(0.f, smc.fadeWeight - smc.fadeRate * dt);
		}
	}

	void AnimationSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();

		auto view = world.View<SkinnedMeshComponent>();
		for (auto entity: view)
		{
			auto& smc = view.get<SkinnedMeshComponent>(entity);
			if (ecs::HasDisabledAncestor(world, World::FromEntt(entity)))
			{
				continue;
			}
			if (!smc.animDb || !smc.animDb->IsValid())
			{
				continue;
			}

			const std::uint32_t clipCount = smc.animDb->GetClipCount();
			if (clipCount == 0)
			{
				continue;
			}

			const float clipDuration = smc.animDb->GetClipDuration(smc.clipIndex);
			const float fadeDuration = smc.fadeClipIndex < clipCount ? smc.animDb->GetClipDuration(smc.fadeClipIndex) : 0.f;
			AdvanceAnimation(smc, dt, clipDuration, fadeDuration);
		}
	}
} // namespace aether
