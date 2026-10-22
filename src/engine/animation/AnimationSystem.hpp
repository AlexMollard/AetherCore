#pragma once

#include <cstdint>
#include <functional>

#include "scene/Entity.hpp"
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

	// Supplies the playing and fading clip durations of one skinned entity when it has no GPU
	// animation database (the browser samples cooked glTF data on the CPU instead). Returns false
	// when the entity has no clips, which skips it exactly like a native entity with an empty database.
	using ClipDurationSource = std::function<bool(World&, Entity, const SkinnedMeshComponent&, float& clipDuration, float& fadeClipDuration)>;

	class AnimationSystem : public System
	{
	public:
		[[nodiscard]] const char* GetName() const override
		{
			return "AnimationSystem";
		}

		void SetClipDurationSource(ClipDurationSource source)
		{
			m_durationSource = std::move(source);
		}

		void Update(World& world, float dt) override;

	private:
		ClipDurationSource m_durationSource;
	};
} // namespace aether
