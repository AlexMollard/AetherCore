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

	// Marks a UI element as focusable/clickable. UiNavigationSystem tracks focus across the
	// active selectables (spatial keyboard nav + mouse hover/click) and sets `focused` /
	// `activated` for scripts. `group`/`interactable` are authored; the rest is runtime.
	struct UISelectable
	{
		std::string group;         // optional grouping; reserved for scoping navigation
		bool interactable = true;  // false = skipped by navigation (locked/redacted items)
		bool focused = false;      // runtime: the currently focused selectable
		bool activated = false;    // runtime: true for the one frame it is activated
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

	// Interactive horizontal slider. One entity; the draw builder renders track+fill+handle
	// and UiWidgetSystem drives value from keyboard (when focused) and mouse drag. Composes a
	// UISelectable for focus (added by the reflection PostSet hook).
	struct UISlider
	{
		float minValue = 0.f;
		float maxValue = 1.f;
		float step = 0.05f; // 0 = continuous
		float value = 0.5f; // real units, clamped to [min,max]
		glm::vec4 trackColor{0.10f, 0.11f, 0.14f, 1.f};
		glm::vec4 fillColor{0.30f, 0.85f, 1.f, 1.f};
		glm::vec4 handleColor{0.30f, 0.85f, 1.f, 1.f};
		float handleRadius = 10.f;
		float cornerRadius = 4.f;
		bool changed = false;  // runtime: user moved value this frame
		bool dragging = false; // runtime: mouse drag in progress
		float pulse = 1.f;     // runtime: focus alpha multiplier for the handle
	};

	struct UIToggle
	{
		bool on = false;
		glm::vec4 trackColor{0.15f, 0.17f, 0.22f, 1.f}; // track when off
		glm::vec4 onColor{0.30f, 0.85f, 1.f, 1.f};      // track when on
		glm::vec4 knobColor{0.90f, 0.95f, 1.f, 1.f};
		float knobRadius = 9.f;
		float cornerRadius = 12.f; // pill
		bool changed = false;      // runtime
		float pulse = 1.f;         // runtime
	};

	struct UIButton
	{
		std::string label;
		std::string fontName = "Roboto";
		float pixelSize = 24.f;
		UIText::HAlign hAlign = UIText::HAlign::Center;
		UIText::VAlign vAlign = UIText::VAlign::Middle;
		glm::vec4 bgColor{0.07f, 0.08f, 0.10f, 1.f};
		glm::vec4 textColor{0.70f, 0.75f, 0.85f, 1.f};
		glm::vec4 bgColorFocused{0.07f, 0.08f, 0.10f, 1.f};
		glm::vec4 textColorFocused{0.30f, 0.85f, 1.f, 1.f};
		float cornerRadius = 4.f;
	};

	// Read-only meter. No interaction, no UISelectable.
	struct UIProgressBar
	{
		float value = 0.f; // normalized 0..1
		glm::vec4 trackColor{0.10f, 0.11f, 0.14f, 1.f};
		glm::vec4 fillColor{0.30f, 0.85f, 1.f, 1.f};
		float cornerRadius = 4.f;
	};

	// A UI element drawn by its OWN shader pipeline ("shaders://<shader>.spv"), on top of the
	// batched UI shapes. The element's UIRect gives the quad; params/colors are handed to the
	// shader as push constants (shader-defined meaning). Used for full-screen effects like the
	// ink transition; created at runtime and never serialized. The batched shape builder ignores
	// entities that carry this, so an effect element emits nothing into ui_shapes.
	struct UIEffect
	{
		std::string shader;      // e.g. "ui_ink" -> shaders://ui_ink.spv
		glm::vec4 params{0.f};   // shader-defined (ink: coverage, time, noiseAmp, edgeWidth)
		glm::vec4 color0{0.f};   // shader-defined (ink: ink body colour)
		glm::vec4 color1{0.f};   // shader-defined (ink: edge/rim colour)
		bool background = false; // true = drawn BEHIND the batched UI (menu backdrops); false = on top (overlays)
	};

	// A custom fragment shader applied to THIS element's own draw commands (its text glyphs, image,
	// or rect) - not a separate quad like UIEffect. The element still uses the shared UI vertex
	// shader, so the custom fragment receives the real glyph/quad geometry + font atlas and the
	// effect is masked to the actual letter shapes. shader "ui_glitch_text" -> shaders://ui_glitch_text.spv.
	struct UIMaterial
	{
		std::string shader;    // fragment shader name (project shader stem)
		glm::vec4 params{0.f}; // shader-defined (x is conventionally time, driven from a script)
		glm::vec4 color0{0.f}; // shader-defined
		glm::vec4 color1{0.f}; // shader-defined
	};
} // namespace aether::ui
