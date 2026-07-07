#pragma once

#include <glm/glm.hpp>

#include "ui/UiComponents.hpp"

namespace aether
{
	class World;
} // namespace aether

namespace aether::ui
{
	// Resolves `rect` against `parentRect` (both (x, y, w, h) in pixels) using
	// the Unity RectTransform anchoring model: anchors place a sub-rect of the
	// parent in normalized [0,1] space, then offsets nudge its edges in pixels.
	// anchorMin == anchorMax collapses the anchor rect to a point (a fixed-size
	// element); anchorMin != anchorMax stretches the element to track the
	// parent's size on that axis. Pure function, no World access - the tree walk
	// that feeds it lives in ResolveCanvases below.
	[[nodiscard]] glm::vec4 ResolveRect(const glm::vec4& parentRect, const UIRect& rect);

	// Resolves UIRect::resolvedRect for every UICanvas root in `world` and its
	// whole HierarchyComponent subtree, in output pixels. Each canvas root's
	// rect is (0, 0, outputExtent.x, outputExtent.y); a descendant with no
	// UIRect passes its parent's resolved rect through unchanged to its own
	// children (so non-UIRect entities can group children without affecting
	// layout).
	void ResolveCanvases(World& world, glm::vec2 outputExtent);
} // namespace aether::ui
