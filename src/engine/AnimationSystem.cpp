#include "AnimationSystem.hpp"

#include <unordered_set>

#include "ModelAnimator.hpp"
#include "Profiler.hpp"
#include "World.hpp"

namespace aether
{
	void AnimationSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		// Deduplicate: multiple primitives of the same model instance share one
		// ModelAnimator pointer. Track which animators have already been ticked
		// this frame so each unique animator advances by exactly one dt.
		std::unordered_set<ModelAnimator*> updated;
		auto view = world.View<AnimatorComponent>();
		for (auto entity: view)
		{
			(void) entity;
			auto& animator = view.get<AnimatorComponent>(entity);
			if (animator.animator && updated.insert(animator.animator).second)
			{
				animator.animator->Update(dt);
			}
		}
	}
} // namespace aether
