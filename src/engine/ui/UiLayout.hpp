#pragma once

#include <algorithm>
#include <glm/glm.hpp>

#include "gpu/GpuEnums.hpp"

namespace aether
{
	// Anchor-based point in screen space.
	// anchor is normalized [0..1] relative to the framebuffer extent.
	// offsetPx is a pixel offset from that anchor.
	struct UiPoint
	{
		glm::vec2 anchor{0.0f, 0.0f};
		glm::vec2 offsetPx{0.0f, 0.0f};
	};

	// Anchor-based rectangle in screen space.
	// Corner positions are resolved as:
	//   min = anchorMin * extent + offsetMinPx
	//   max = anchorMax * extent + offsetMaxPx
	struct UiRect
	{
		glm::vec2 anchorMin{0.0f, 0.0f};
		glm::vec2 anchorMax{0.0f, 0.0f};
		glm::vec2 offsetMinPx{0.0f, 0.0f};
		glm::vec2 offsetMaxPx{0.0f, 0.0f};
	};

	[[nodiscard]] inline glm::vec2 ResolveUiPointPx(gpu::Extent2D extent, const UiPoint& point)
	{
		const glm::vec2 sizePx{static_cast<float>(extent.width), static_cast<float>(extent.height)};
		return point.anchor * sizePx + point.offsetPx;
	}

	// Returns x, y, width, height in pixels. Width/height are clamped to >= 0.
	[[nodiscard]] inline glm::vec4 ResolveUiRectPx(gpu::Extent2D extent, const UiRect& rect)
	{
		const glm::vec2 sizePx{static_cast<float>(extent.width), static_cast<float>(extent.height)};
		const glm::vec2 minPos = rect.anchorMin * sizePx + rect.offsetMinPx;
		const glm::vec2 maxPos = rect.anchorMax * sizePx + rect.offsetMaxPx;

		const float x0 = std::min(minPos.x, maxPos.x);
		const float y0 = std::min(minPos.y, maxPos.y);
		const float x1 = std::max(minPos.x, maxPos.x);
		const float y1 = std::max(minPos.y, maxPos.y);

		return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
	}

	// -- Anchor presets --------------------------------------------------------
	// Factory helpers that produce a UiRect from a named anchor point + size.
	//
	// Fixed presets (anchorMin == anchorMax -> widget does not resize with screen):
	//   TopLeft / TopCenter / TopRight
	//   MiddleLeft / Center / MiddleRight
	//   BottomLeft / BottomCenter / BottomRight
	//
	//   marginPx  - pixel gap between the named screen corner/edge and the widget.
	//               For Center, this is an additional offset from the screen midpoint.
	//   sizePx    - widget dimensions in pixels.
	//
	// Stretch presets (anchorMin != anchorMax -> widget scales with the screen):
	//   StretchFull        - fills the entire screen with an optional uniform inset.
	//   StretchHorizontal  - full-width band; caller supplies the Y position and height.
	//   StretchVertical    - full-height column; caller supplies the X position and width.

	namespace UiAnchors
	{
		// Internal helper: both anchor corners identical -> fixed-size widget.
		[[nodiscard]] inline UiRect Fixed(glm::vec2 anchor, glm::vec2 offsetMinPx, glm::vec2 sizePx)
		{
			return UiRect{.anchorMin = anchor, .anchorMax = anchor, .offsetMinPx = offsetMinPx, .offsetMaxPx = offsetMinPx + sizePx};
		}

		// -- Fixed-position presets ---------------------------------------------

		[[nodiscard]] inline UiRect TopLeft(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			return Fixed({0.f, 0.f}, marginPx, sizePx);
		}

		[[nodiscard]] inline UiRect TopCenter(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			// Anchor at top-centre; shift left by half the width so the widget is centred.
			return Fixed({0.5f, 0.f}, glm::vec2{-sizePx.x * 0.5f + marginPx.x, marginPx.y}, sizePx);
		}

		[[nodiscard]] inline UiRect TopRight(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			// Anchor at top-right; offsets are negative to place the widget to the left.
			return Fixed({1.f, 0.f}, glm::vec2{-sizePx.x - marginPx.x, marginPx.y}, sizePx);
		}

		[[nodiscard]] inline UiRect MiddleLeft(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			return Fixed({0.f, 0.5f}, glm::vec2{marginPx.x, -sizePx.y * 0.5f + marginPx.y}, sizePx);
		}

		[[nodiscard]] inline UiRect Center(glm::vec2 sizePx, glm::vec2 offsetPx = {})
		{
			// Anchor at screen centre; subtract half size so the widget is centred.
			const glm::vec2 min = offsetPx - sizePx * 0.5f;
			return Fixed({0.5f, 0.5f}, min, sizePx);
		}

		[[nodiscard]] inline UiRect MiddleRight(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			return Fixed({1.f, 0.5f}, glm::vec2{-sizePx.x - marginPx.x, -sizePx.y * 0.5f + marginPx.y}, sizePx);
		}

		[[nodiscard]] inline UiRect BottomLeft(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			return Fixed({0.f, 1.f}, glm::vec2{marginPx.x, -sizePx.y - marginPx.y}, sizePx);
		}

		[[nodiscard]] inline UiRect BottomCenter(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			return Fixed({0.5f, 1.f}, glm::vec2{-sizePx.x * 0.5f + marginPx.x, -sizePx.y - marginPx.y}, sizePx);
		}

		[[nodiscard]] inline UiRect BottomRight(glm::vec2 marginPx, glm::vec2 sizePx)
		{
			return Fixed({1.f, 1.f}, glm::vec2{-sizePx.x - marginPx.x, -sizePx.y - marginPx.y}, sizePx);
		}

		// -- Stretch presets ----------------------------------------------------

		// Fills the screen; `inset` shrinks each edge uniformly.
		[[nodiscard]] inline UiRect StretchFull(float inset = 0.f)
		{
			return UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {1.f, 1.f}, .offsetMinPx = {inset, inset}, .offsetMaxPx = {-inset, -inset}};
		}

		// Full-width horizontal band.  `yPx` and `heightPx` are in pixels from the top.
		[[nodiscard]] inline UiRect StretchHorizontal(float yPx, float heightPx, float insetX = 0.f)
		{
			return UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {1.f, 0.f}, .offsetMinPx = {insetX, yPx}, .offsetMaxPx = {-insetX, yPx + heightPx}};
		}

		// Full-height vertical column.  `xPx` and `widthPx` are in pixels from the left.
		[[nodiscard]] inline UiRect StretchVertical(float xPx, float widthPx, float insetY = 0.f)
		{
			return UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {0.f, 1.f}, .offsetMinPx = {xPx, insetY}, .offsetMaxPx = {xPx + widthPx, -insetY}};
		}

	} // namespace UiAnchors

} // namespace aether
