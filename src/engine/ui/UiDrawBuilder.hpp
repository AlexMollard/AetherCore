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

	// Walks every UICanvas subtree pre-order, appending one command per UIImage
	// found and one command per shaped UIText glyph when a FontRegistry is
	// supplied. layer is set to a running emission index, encoding painter order
	// (parent behind child) so the GPU can render in a stable order without a
	// separate sort.
	// Assumes UiLayoutSystem::ResolveCanvases already ran this frame - reads
	// UIRect::resolvedRect, does not resolve layout itself.
	void BuildDrawCommands(World& world, std::vector<UiDrawCommand>& out, FontRegistry* fonts = nullptr, const TextureRegistry* textures = nullptr);
} // namespace aether::ui
