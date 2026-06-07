#include "animation/AnimationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void AnimationSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE_N("AnimationSystem.Update");

		auto view = world.View<SkinnedMeshComponent>();
		for (auto entity: view)
		{
			auto& smc = view.get<SkinnedMeshComponent>(entity);
			if (!smc.animDb || !smc.animDb->IsValid())
			{
				continue;
			}

			if (smc.animDb->GetClipCount() == 0)
			{
				continue;
			}

			smc.animTime += dt * smc.playbackSpeed;
			if (smc.looping)
			{
				const float dur = smc.animDb->GetClipDuration(smc.clipIndex);
				if (dur > 0.f && smc.animTime > dur)
				{
					smc.animTime = std::fmod(smc.animTime, dur);
				}
			}
		}

		auto blendView = world.View<AnimationBlendComponent>();
		for (auto [entity, blendComp]: blendView.each())
		{
			if (!blendComp.inTransition)
			{
				continue;
			}

			blendComp.blendWeight -= blendComp.transitionSpeed * dt;
			if (blendComp.blendWeight <= 0.0f)
			{
				blendComp.blendWeight = 1.0f;
				blendComp.primaryClip = blendComp.secondaryClip;
				blendComp.secondaryClip = 0;
				blendComp.inTransition = false;
			}
		}
	}
} // namespace aether
