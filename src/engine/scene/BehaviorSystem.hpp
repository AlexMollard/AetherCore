#pragma once

#include "scene/System.hpp"

namespace aether
{
	class AssetManager;

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
} // namespace aether
