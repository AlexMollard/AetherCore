#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "Entity.hpp"
#include "UiLayout.hpp"

namespace aether::ui
{
	// ── Transform ─────────────────────────────────────────────────────────────
	// Screen-space position, size, and Z ordering for a UI entity.
	struct UiTransformComponent
	{
		UiRect rect;        // anchor-based screen rect; UiSystem updates offsetPx during drag
		float zOrder = 0.f; // higher = drawn and hit-tested on top
	};

	// ── Visuals ────────────────────────────────────────────────────────────────
	// Background quad appearance.
	struct UiRenderComponent
	{
		glm::vec4 backgroundColor{ 0.f, 0.f, 0.f, 0.f };
		glm::vec4 borderColor{ 0.f, 0.f, 0.f, 0.f };
		float cornerRadius = 0.f;
		float borderWidth = 0.f;
		bool visible = true;
	};

	// Text label attached to a widget.
	struct UiTextComponent
	{
		std::string text;
		float fontSize = 14.f;
		glm::vec4 color{ 1.f, 1.f, 1.f, 1.f };
	};

	// ── Interaction ────────────────────────────────────────────────────────────
	// Marks an entity as interactive. UiSystem writes hover/press/clicked state
	// here every frame; widgets read it in OnGui to drive visual changes.
	struct UiInputComponent
	{
		bool hovered = false;   // cursor is over this entity's rect
		bool pressed = false;   // left button held while this is the active entity
		bool clicked = false;   // true for ONE frame when released over this entity
		bool focused = false;   // keyboard focus
		bool blockInput = true; // stops hit-testing from passing through to entities below
	};

	// ── Panel ──────────────────────────────────────────────────────────────────
	// Attaches panel semantics (title bar, drag, collapse) to any entity that
	// also has UiTransformComponent + UiInputComponent.
	struct UiPanelComponent
	{
		std::string title;
		bool draggable = true;
		bool collapsible = false;
		bool collapsed = false;

		// Set by UiSystem when a drag starts; used to compute rect updates.
		glm::vec2 dragStartMin{};
		glm::vec2 dragStartMax{};
	};

	// ── Button ─────────────────────────────────────────────────────────────────
	struct UiButtonComponent
	{
		std::string label;
		glm::vec4 normalColor{ 0.20f, 0.24f, 0.30f, 1.f };
		glm::vec4 hoverColor{ 0.28f, 0.34f, 0.44f, 1.f };
		glm::vec4 pressColor{ 0.16f, 0.20f, 0.26f, 1.f };
		glm::vec4 textColor{ 0.88f, 0.91f, 0.93f, 1.f };
	};

	// ── Slider ─────────────────────────────────────────────────────────────────
	struct UiSliderComponent
	{
		float min = 0.f;
		float max = 1.f;
		float value = 0.f;
		bool isDragging = false;
	};

	// ── Checkbox ───────────────────────────────────────────────────────────────
	struct UiCheckboxComponent
	{
		bool checked = false;
		std::string label;
	};

	// ── Image ──────────────────────────────────────────────────────────────────
	// Displays a bindless texture on a quad.
	struct UiImageComponent
	{
		std::uint32_t textureSlot = 0;
		glm::vec4 uvRect{ 0.f, 0.f, 1.f, 1.f }; // u0, v0, u1, v1
		glm::vec4 tint{ 1.f, 1.f, 1.f, 1.f };
	};

	// ── Clip ───────────────────────────────────────────────────────────────────
	// Marks a container as clipping its children; currently performs CPU-side
	// culling in UIRenderer. Scroll offset shifts child content.
	struct UiClipComponent
	{
		UiRect clipRect;
		glm::vec2 scrollOffset{ 0.f };
	};

	// ── Layout ─────────────────────────────────────────────────────────────────
	// Drives automatic child positioning within a container.
	struct UiLayoutComponent
	{
		enum class Direction
		{
			Vertical,
			Horizontal
		} direction = Direction::Vertical;
		float spacing = 4.f;
		float padding = 8.f;
	};

	// ── Hierarchy ──────────────────────────────────────────────────────────────
	struct UiChildrenComponent
	{
		std::vector<Entity> children;
	};

	struct UiParentComponent
	{
		Entity parent;
	};

} // namespace aether::ui
