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
} // namespace aether::ui
