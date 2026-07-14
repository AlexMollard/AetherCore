#pragma once

#include <vector>

#include "ui/UiDrawCommand.hpp"

namespace aether
{
	class TextureRegistry;
	class World;
} // namespace aether

namespace aether::ui
{
	class FontRegistry;

	// UIRect::resolvedRect, does not resolve layout itself.
	void BuildDrawCommands(World& world, std::vector<UiDrawCommand>& out, FontRegistry* fonts = nullptr, const TextureRegistry* textures = nullptr);
} // namespace aether::ui
