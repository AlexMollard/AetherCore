#pragma once

#include "scene/System.hpp"

namespace aether
{
	class AnimationSystem : public System
	{
	public:
		[[nodiscard]] const char* GetName() const override
		{
			return "AnimationSystem";
		}

		[[nodiscard]] SceneFeatureFlags RequiredFeatures() const override
		{
			return SceneFeatureFlags::Meshes3D;
		}

		void Update(World& world, float dt) override;
	};
} // namespace aether
