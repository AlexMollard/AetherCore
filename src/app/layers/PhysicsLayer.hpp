#pragma once

#include "AppLayer.hpp"

namespace aether::app
{
	class PhysicsGameSystem;

	// Demonstration layer for Jolt physics integration.
	// PhysicsSystem is engine-owned (registered in Application::Run); this layer
	// only drives the PhysicsGameSystem (scene creation, input, HUD).
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
		PhysicsGameSystem* m_gameSystem = nullptr;
	};

} // namespace aether::app
