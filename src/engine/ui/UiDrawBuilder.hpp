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

	// UIRect::resolvedRect, does not resolve layout itself. `materials` is filled with one entry
	// per UIMaterial-tagged element encountered (index+1 == the commands' shaderId); pass an unused
	// vector if custom UI materials are not needed.
	void BuildDrawCommands(World& world, std::vector<UiDrawCommand>& out, std::vector<UiMaterialDraw>& materials, FontRegistry* fonts = nullptr, TextureRegistry* textures = nullptr);
} // namespace aether::ui
