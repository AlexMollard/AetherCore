#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"

namespace aether::ui
{
	// Roots a UI subtree for layout resolution. UiLayoutSystem::ResolveCanvases
	struct UICanvas
	{
		enum class ScaleMode : std::uint8_t
		{
			ConstantPixel,
			ScaleWithReference
		};

		ScaleMode scaleMode = ScaleMode::ConstantPixel;
		glm::vec2 referenceResolution{1920.f, 1080.f};
		int sortBias = 0;
	};

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
		TextureHandle texture{};
		// Authored texture path; resolved to `texture` lazily by the draw builder when
		// `textureDirty` is set. Every other texture-bearing component stores the path
		// (SpriteRenderer, TileMap); this brings UIImage in line so reflection/MCP can set it.
		std::string texturePath;
		bool textureDirty = false;
		// Pixel-art sampling: the UI shader snaps UVs to texel centres.
		bool pixelArt = false;
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
