#include "AnimationSystem.hpp"

#include "World.hpp"
#include "ModelAnimator.hpp"

namespace aether
{
	void AnimationSystem::Update(World& world, float dt)
	{
		// Iterate over all entities with an animator and update their animations.
		auto view = world.View<AnimatorComponent>();
		for (auto entity : view)
		{
			(void)entity;
			auto& animator = view.get<AnimatorComponent>(entity);
			if (animator.animator)
			{
				animator.animator->Update(dt);
			}
		}
	}
}
