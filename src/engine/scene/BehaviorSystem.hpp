#pragma once

#include "scene/System.hpp"

namespace aether
{
	class AssetManager;
} // namespace aether

namespace aether::app
{
	// Advances the data-driven scene behaviors (Bob/Spin/Orbit/MaterialPulse -
	// scene/BehaviorComponents.hpp). Registered with the World's system list,
	// so the editor's play gate (Application::OnUpdate skipping UpdateSystems
	// while Editing) freezes it together with physics and animation.
	class BehaviorSystem final : public System
	{
	public:
		explicit BehaviorSystem(AssetManager& assets)
		      : m_assets(assets)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "BehaviorSystem";
		}

		void Update(World& world, float dt) override;

	private:
		AssetManager& m_assets;
	};
} // namespace aether::app
