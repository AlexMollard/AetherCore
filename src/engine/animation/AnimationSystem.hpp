#pragma once

#include <cstdint>

#include "scene/System.hpp"

namespace aether
{
	struct SkinnedMeshComponent;

	// Starts `clip` from time 0 while the clip playing now keeps its own clock and fades out over
	// `seconds`. A request mid-fade fades out whichever clip currently shows more. seconds <= 0
	// switches outright, like setting clipIndex/animTime directly.
	void CrossFadeTo(SkinnedMeshComponent& smc, std::uint32_t clip, float seconds);

	// Advances the playing clip's clock and any fade by dt. The durations wrap looping clips.
	void AdvanceAnimation(SkinnedMeshComponent& smc, float dt, float clipDuration, float fadeClipDuration);

	class AnimationSystem : public System
	{
	public:
		[[nodiscard]] const char* GetName() const override
		{
			return "AnimationSystem";
		}

		void Update(World& world, float dt) override;
	};
} // namespace aether
