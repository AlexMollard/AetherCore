#pragma once

#include "scene/Entity.hpp"
#include "utils/Ray.hpp"

namespace aether
{
	class PhysicsSystem;
	class World;
} // namespace aether

namespace aether::editor
{
	struct PickHit
	{
		Entity entity{};
		float t = 0.0f;

		[[nodiscard]] bool IsValid() const
		{
			return entity.IsValid();
		}
	};

	// Nearest entity under the world-space ray (dir must be normalized): exact
	// OBB test against every mesh entity's local AABB, merged with a physics
	// raycast when `physics` is provided - the closer hit wins.
	PickHit PickEntity(World& world, PhysicsSystem* physics, const Ray& ray, float maxDist);
} // namespace aether::editor
