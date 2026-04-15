#include "AnimationSystem.hpp"

#include "World.hpp"
#include "ModelAnimator.hpp"

namespace aether
{
	void AnimationSystem::Update(World& world, float dt)
	{
		// Iterate over all entities with an animator and update their animations.
		world.ForEachAnimator([dt](Entity entity, AnimatorComponent& animator)
		{
			(void)entity;  // unused, but useful for future debugging
			if (animator.animator)
			{
				animator.animator->Update(dt);
			}
		});
	}
}
