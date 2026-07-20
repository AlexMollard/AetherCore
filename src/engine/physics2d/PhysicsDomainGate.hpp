#pragma once

namespace aether
{
	class World;
	struct Entity;

	// Entity-level physics domain checks. Scene-level policy lives in
	// AllowedSceneFeatures (scene/SceneKind.hpp) and feature-driven system
	// activation (System::RequiredFeatures); these helpers answer the one
	// remaining question both physics flushes and the authoring layer share:
	// does this entity already belong to the other physics domain?
	namespace physics_gate
	{
		[[nodiscard]] bool EntityHas2DPhysics(const World& world, Entity entity);
		[[nodiscard]] bool EntityHas3DPhysics(const World& world, Entity entity);
	} // namespace physics_gate
} // namespace aether
