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

		void Update(World& world, float dt) override;
	};
} // namespace aether
