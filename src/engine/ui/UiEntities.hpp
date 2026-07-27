#pragma once

#include <string>

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
	Entity CreateTextBoxEntity(World& world, Entity canvas);
	Entity CreateProgressBarEntity(World& world, Entity canvas);
	// Full-screen element rendered by its own shader ("shaders://<shader>.spv").
	Entity CreateEffectEntity(World& world, Entity canvas, const std::string& shader);
} // namespace aether::ui
