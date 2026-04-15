#pragma once

#include <algorithm>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
	// Anchor-based point in screen space.
	// anchor is normalized [0..1] relative to the framebuffer extent.
	// offsetPx is a pixel offset from that anchor.
	struct UiPoint
	{
		glm::vec2 anchor{ 0.0f, 0.0f };
		glm::vec2 offsetPx{ 0.0f, 0.0f };
	};

	// Anchor-based rectangle in screen space.
	// Corner positions are resolved as:
	//   min = anchorMin * extent + offsetMinPx
	//   max = anchorMax * extent + offsetMaxPx
	struct UiRect
	{
		glm::vec2 anchorMin{ 0.0f, 0.0f };
		glm::vec2 anchorMax{ 0.0f, 0.0f };
		glm::vec2 offsetMinPx{ 0.0f, 0.0f };
		glm::vec2 offsetMaxPx{ 0.0f, 0.0f };
	};

	[[nodiscard]] inline glm::vec2 ResolveUiPointPx(VkExtent2D extent, const UiPoint& point)
	{
		const glm::vec2 sizePx{ static_cast<float>(extent.width), static_cast<float>(extent.height) };
		return point.anchor * sizePx + point.offsetPx;
	}

	// Returns x, y, width, height in pixels. Width/height are clamped to >= 0.
	[[nodiscard]] inline glm::vec4 ResolveUiRectPx(VkExtent2D extent, const UiRect& rect)
	{
		const glm::vec2 sizePx{ static_cast<float>(extent.width), static_cast<float>(extent.height) };
		const glm::vec2 minPos = rect.anchorMin * sizePx + rect.offsetMinPx;
		const glm::vec2 maxPos = rect.anchorMax * sizePx + rect.offsetMaxPx;

		const float x0 = std::min(minPos.x, maxPos.x);
		const float y0 = std::min(minPos.y, maxPos.y);
		const float x1 = std::max(minPos.x, maxPos.x);
		const float y1 = std::max(minPos.y, maxPos.y);

		return glm::vec4(x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0));
	}
} // namespace aether
