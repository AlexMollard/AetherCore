#pragma once

#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace aether
{
	// Converts an authored sprite collision outline (pixel space, origin at the
	// sprite's top-left, +y down - the SpriteRegion::collisionOutline convention)
	// into Collider2D local points (world units, pivot-relative, +y up), matching
	// how the sprite quad itself maps pixels to world space. Returns empty when
	// the outline has fewer than 3 points. Box2D caps convex hulls at 8 vertices;
	// callers should surface a warning when the outline exceeds that (the hull
	// build drops the extras).
	[[nodiscard]] std::vector<glm::vec2> BuildColliderPointsFromOutline(std::span<const glm::vec2> outlinePixels, glm::vec2 spritePixelSize, glm::vec2 pivot, float pixelsPerUnit);
} // namespace aether
