#include "physics2d/SpriteColliderGen.hpp"

#include <algorithm>

namespace aether
{
	std::vector<glm::vec2> BuildColliderPointsFromOutline(std::span<const glm::vec2> outlinePixels, glm::vec2 spritePixelSize, glm::vec2 pivot, float pixelsPerUnit)
	{
		if (outlinePixels.size() < 3 || pixelsPerUnit <= 0.0f)
		{
			return {};
		}
		const glm::vec2 size = glm::max(spritePixelSize, glm::vec2(1.0f));
		std::vector<glm::vec2> points;
		points.reserve(outlinePixels.size());
		for (const glm::vec2& p: outlinePixels)
		{
			// Pixel origin is the sprite's top-left with +y down; collider space is
			// pivot-relative with +y up, and the pivot is measured from the sprite's
			// BOTTOM-left (the quad's corner space: local = (corner - pivot) * size).
			points.emplace_back((p.x - pivot.x * size.x) / pixelsPerUnit, (size.y - p.y - pivot.y * size.y) / pixelsPerUnit);
		}
		return points;
	}
} // namespace aether
