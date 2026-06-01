#pragma once

#include "AppLayer.hpp"

namespace aether::app
{
	class PhysicsGameSystem;

	class PhysicsLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		PhysicsGameSystem* m_gameSystem = nullptr;

		// Cached display values updated in OnUpdate
		int m_activeBodyCount = 0;
		int m_projectileCount = 0;
		float m_simTime = 0.f;
	};
} // namespace aether::app
