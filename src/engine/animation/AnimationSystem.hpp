#pragma once

#include "scene/System.hpp"

namespace aether
{
	// Example system: updates all skinned entities with animation data.
	// This demonstrates how game logic systems integrate with the ECS.
	class AnimationSystem : public System
	{
	public:
		const char* GetName() const override
		{
			return "AnimationSystem";
		}

		void Update(World& world, float dt) override;
	};
} // namespace aether
