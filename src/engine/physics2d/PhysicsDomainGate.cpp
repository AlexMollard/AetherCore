#include "physics2d/PhysicsDomainGate.hpp"

#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/World.hpp"

namespace aether::physics_gate
{
	bool EntityHas2DPhysics(const World& world, Entity entity)
	{
		// Only a rigid body commits an entity to a domain. A stray collider or
		// joint of the other domain used to flip this check too, which silently
		// dropped the entity from BOTH simulations: each flush refused it because
		// the other domain's component was present, while neither domain actually
		// owned a second rigid body on it.
		return world.Has<RigidBody2DComponent>(entity);
	}

	bool EntityHas3DPhysics(const World& world, Entity entity)
	{
		return world.Has<RigidBodyComponent>(entity);
	}
} // namespace aether::physics_gate
