#pragma once

#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::ui
{
	Entity CreateCanvasEntity(World& world);
	Entity CreateImageEntity(World& world, Entity canvas);
	Entity CreateTextEntity(World& world, Entity canvas);
	Entity CreateSliderEntity(World& world, Entity canvas);
	Entity CreateToggleEntity(World& world, Entity canvas);
	Entity CreateButtonEntity(World& world, Entity canvas);
	Entity CreateProgressBarEntity(World& world, Entity canvas);
} // namespace aether::ui
