#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"

namespace aether::ui
{
	// Roots a UI subtree for layout resolution. UiLayoutSystem::ResolveCanvases
	// walks every entity carrying both UICanvas and UIRect, treating its
	// resolved rect as (0, 0, outputExtent.x, outputExtent.y) and resolving the
	// whole HierarchyComponent subtree beneath it against that rect.
	struct UICanvas
	{
		enum class ScaleMode : std::uint8_t
		{
			ConstantPixel,     // 1 UI unit == 1 output pixel
			ScaleWithReference // UI units scale so referenceResolution fills the output
		};

		ScaleMode scaleMode = ScaleMode::ConstantPixel;
		glm::vec2 referenceResolution{1920.f, 1080.f};
		int sortBias = 0;
	};

	// Unity RectTransform-style anchored rect. Anchors place a sub-rect of the
	// parent in normalized [0,1] space; offsets then nudge its edges in pixels.
	// anchorMin == anchorMax collapses the anchor rect to a point, giving a
	// fixed-size element positioned by offsetMin/offsetMax around it. Differing
	// anchors stretch the element to track the parent's size on that axis, with
	// offsetMin/offsetMax read as inward margins from the anchored edges.
	struct UIRect
	{
		glm::vec2 anchorMin{0.5f, 0.5f};
		glm::vec2 anchorMax{0.5f, 0.5f};
		glm::vec2 offsetMin{-50.f, -50.f};
		glm::vec2 offsetMax{50.f, 50.f};
		glm::vec2 pivot{0.5f, 0.5f};
		glm::vec4 resolvedRect{0.f}; // (x, y, w, h) output px; runtime only, never serialized
	};

	struct UIImage
	{
		glm::vec4 color{1.f};
		float cornerRadius = 0.f;
		TextureHandle texture{}; // invalid handle => solid color fill
	};

	struct UIText
	{
		enum class HAlign : std::uint8_t
		{
			Left,
			Center,
			Right
		};

		enum class VAlign : std::uint8_t
		{
			Top,
			Middle,
			Bottom
		};

		std::string text;
		std::string fontName = "Roboto";
		float pixelSize = 24.f;
		glm::vec4 color{1.f};
		HAlign hAlign = HAlign::Left;
		VAlign vAlign = VAlign::Top;
		bool wrap = true;
	};
} // namespace aether::ui
