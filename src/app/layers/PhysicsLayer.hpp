#pragma once

#include "AppLayer.hpp"
#include "physics/PhysicsSystem.hpp"

namespace aether::app
{
	class PhysicsGameSystem;

	// Demonstration layer for Jolt physics integration.
	// Owns the PhysicsSystem (registered into the World) and the PhysicsGameSystem.
	// Controls:
	//   Space  - fire a projectile at the box stack
	//   R      - reset the scene
	//   C      - toggle orbit / free camera
	class PhysicsLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		aether::PhysicsSystem* m_physics = nullptr;
		PhysicsGameSystem* m_gameSystem = nullptr;
	};

} // namespace aether::app
