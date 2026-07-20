#include "physics2d/PhysicsDomainGate.hpp"

#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/World.hpp"

namespace aether::physics_gate
{
	bool EntityHas2DPhysics(const World& world, Entity entity)
	{
		return world.Has<RigidBody2DComponent>(entity) || world.Has<Collider2DComponent>(entity) || world.Has<Joint2DComponent>(entity);
	}

	bool EntityHas3DPhysics(const World& world, Entity entity)
	{
		return world.Has<RigidBodyComponent>(entity) || world.Has<ColliderComponent>(entity) || world.Has<JointComponent>(entity);
	}
} // namespace aether::physics_gate
