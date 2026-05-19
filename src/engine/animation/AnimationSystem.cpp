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

			const std::uint32_t clipCount = smc.animDb->GetClipCount();
			if (clipCount == 0)
			{
				continue;
			}

			const std::uint32_t clip = std::min(smc.clipIndex, clipCount - 1u);
			smc.animTime += dt * smc.playbackSpeed;

			if (smc.looping)
			{
				const float dur = smc.animDb->GetClipDuration(clip);
				if (dur > 0.f && smc.animTime > dur)
				{
					smc.animTime = std::fmod(smc.animTime, dur);
				}
			}
		}
	}
} // namespace aether
