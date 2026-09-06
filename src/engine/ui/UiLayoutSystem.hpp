#pragma once

#include <glm/glm.hpp>

#include "ui/UiComponents.hpp"

namespace aether
{
	class World;
}

namespace aether::ui
{
	// UI layout is Y-DOWN at every layer: anchor.y = 0 is the TOP of the parent, 1.0 is
	// the BOTTOM (UiLayoutSystem.cpp's ResolveRect: anchorMinPx = parentMin + anchorMin *
	// parentSize, and the root canvas's parentMin is {0,0}); offsetY follows the same
	// convention, so for pivot.y = 1.0 a MORE NEGATIVE offsetY renders HIGHER on screen.
	// Confirmed at the pixel, not just inferred from other screens: ui_shapes.slang's
	// vertex shader computes ndcY = (sp.y / screenSize.y) * 2.0 - 1.0 through a plain,
	// unflipped Vulkan viewport (RenderGraph.cpp's per-pass viewport has no negative
	// height), so sp.y = 0 is the top row and sp.y = screenSize.y is the bottom row.
	// This has been independently re-derived three times in one session (from a working
	// screen's offsets, from a different screen's comments, and from this trace) - read
	// this comment before deriving a fourth.
	[[nodiscard]] glm::vec4 ResolveRect(const glm::vec4& parentRect, const UIRect& rect);

	// layout).
	void ResolveCanvases(World& world, glm::vec2 outputExtent);
} // namespace aether::ui
