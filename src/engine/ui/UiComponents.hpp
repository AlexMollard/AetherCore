#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "scene/Entity.hpp"
#include "ui/UiLayout.hpp"

namespace aether::ui
{
	// -- Transform -------------------------------------------------------------
	// Screen-space position, size, and Z ordering for a UI entity.
	struct UiTransformComponent
	{
		UiRect rect{};        // anchor-based screen rect; UiSystem updates offsetPx during drag
		float zOrder = 0.f;   // higher = drawn and hit-tested on top
		float flexGrow = 0.f; // 0 = fixed size; > 0 = takes proportional share of remaining layout space
	};

	// -- Visuals ----------------------------------------------------------------
	// Background quad appearance.
	struct UiRenderComponent
	{
		glm::vec4 backgroundColor{0.f, 0.f, 0.f, 0.f};
		glm::vec4 borderColor{0.f, 0.f, 0.f, 0.f};
		float cornerRadius = 0.f;
		float borderWidth = 0.f;
		bool visible = true;
	};

	// Text label attached to a widget.
	struct UiTextComponent
	{
		std::string text;
		float fontSize = 14.f;
		glm::vec4 color{1.f, 1.f, 1.f, 1.f};
	};

	// -- Interaction ------------------------------------------------------------
	// Marks an entity as interactive. UiSystem writes hover/press/clicked state
	// here every frame; widgets read it in OnGui to drive visual changes.
	struct UiInputComponent
	{
		bool hovered = false;   // cursor is over this entity's rect
		bool pressed = false;   // left button held while this is the active entity
		bool clicked = false;   // true for ONE frame when released over this entity
		bool focused = false;   // keyboard focus
		bool blockInput = true; // stops hit-testing from passing through to entities below

		// Screen-space pixel position of the most recent click (set when clicked==true).
		// Widgets that need click-location logic (e.g. panel header vs body) read this.
		glm::vec2 clickPos{};

		// Animated blend weights updated by UiSystem each frame (range [0..1]).
		// UiSystem lerps these toward 1 when the flag is true, toward 0 otherwise,
		// using a fixed 80 ms transition time so widgets can do smooth color blends.
		float hoverT = 0.f;
		float pressT = 0.f;
	};

	// -- Panel ------------------------------------------------------------------
	// Attaches panel semantics (title bar, drag, collapse) to any entity that
	// also has UiTransformComponent + UiInputComponent.
	struct UiPanelComponent
	{
		std::string title;
		bool draggable = true;
		bool collapsible = false;
		bool collapsed = false;
		float headerExtensionHeight = 0.f;
		float bodyScrollY = 0.f;
		float bodyMaxScrollY = 0.f;

		// Full-size rect saved when the panel collapses; restored on expand.
		// Written by DrawPanel - do not set manually.
		UiRect expandedRect{};

		// Set by UiSystem when a drag starts; used to compute rect updates.
		glm::vec2 dragStartMin{};
		glm::vec2 dragStartMax{};
	};

	// -- Button -----------------------------------------------------------------
	struct UiButtonComponent
	{
		std::string label;
		glm::vec4 normalColor{0.20f, 0.24f, 0.30f, 1.f};
		glm::vec4 hoverColor{0.28f, 0.34f, 0.44f, 1.f};
		glm::vec4 pressColor{0.16f, 0.20f, 0.26f, 1.f};
		glm::vec4 textColor{0.88f, 0.91f, 0.93f, 1.f};
	};

	// -- Slider -----------------------------------------------------------------
	struct UiSliderComponent
	{
		float min = 0.f;
		float max = 1.f;
		float value = 0.f;
		bool isDragging = false;
		std::string label;
	};

	// -- Checkbox ---------------------------------------------------------------
	struct UiCheckboxComponent
	{
		bool checked = false;
		std::string label;
	};

	// -- Image ------------------------------------------------------------------
	// Displays a bindless texture on a quad.
	struct UiImageComponent
	{
		std::uint32_t textureSlot = 0;
		glm::vec4 uvRect{0.f, 0.f, 1.f, 1.f}; // u0, v0, u1, v1
		glm::vec4 tint{1.f, 1.f, 1.f, 1.f};
	};

	// -- Clip -------------------------------------------------------------------
	// Marks a container as clipping its children; currently performs CPU-side
	// culling in UIRenderer. Scroll offset shifts child content.
	struct UiClipComponent
	{
		UiRect clipRect;
		glm::vec2 scrollOffset{0.f};
	};

	// -- Layout -----------------------------------------------------------------
	// Drives automatic child positioning within a container.
	struct UiLayoutComponent
	{
		enum class Direction
		{
			Vertical,
			Horizontal
		} direction = Direction::Vertical;

		enum class Alignment
		{
			Start, // top for horizontal, left for vertical
			Center,
			End,     // bottom for horizontal, right for vertical
			Stretch, // fill the cross-axis (default)
		};

		float spacing = 4.f;
		float padding = 8.f;
		// When true, ApplyLayout shrinks/grows the container to exactly wrap its
		// children (plus padding), so panels become self-sizing without a fixed rect.
		bool autoSize = false;
		// When false, DrawChildren skips this container's subtree. Tab bars use
		// this to hide non-selected pages without modifying rects.
		bool visible = true;
		Alignment crossAlignment = Alignment::Stretch;
	};

	// -- Text Input -------------------------------------------------------------
	// Editable single-line text field.  UiSystem feeds typed characters and key
	// events when this entity has keyboard focus (UiInputComponent::focused).
	struct UiTextInputComponent
	{
		std::string text;
		std::string placeholder;
		int cursorPos = 0; // byte index into text; kept in sync by UiSystem
		float cursorBlinkTime = 0.f;
		bool cursorVisible = true;
		int maxLength = 256;    // character limit; 0 = unlimited
		bool submitted = false; // true for ONE frame when Enter is pressed; DrawTextInput clears it
	};

	// -- Drag vector ------------------------------------------------------------
	// Editor-style XYZ numeric control. Drag horizontally on a component to adjust
	// it, or click without dragging to type a numeric value.
	struct UiVec3DragComponent
	{
		std::string label;
		glm::vec3 value{0.f};
		glm::vec3 min{-1000000.f};
		glm::vec3 max{1000000.f};
		float speed = 0.05f;
		int decimals = 2;
		int activeAxis = -1;
		int editingAxis = -1;
		glm::vec2 dragStartMouse{};
		glm::vec3 dragStartValue{0.f};
		std::string editText;
		bool dragging = false;
		bool changed = false;
		bool visible = true;
		bool readOnly = false;
	};

	// -- Grid Layout ------------------------------------------------------------
	// Positions UiChildrenComponent children in a uniform cell grid and
	// auto-sizes the container height to wrap all rows.
	struct UiGridLayoutComponent
	{
		int columns = 8;
		float slotSize = 50.f; // both width and height of each cell
		float spacing = 4.f;   // gap between cells (horizontal and vertical)
		float padding = 8.f;   // inset from the container edge on all sides
	};

	// -- Item Slot ---------------------------------------------------------------
	// Represents one inventory cell drawn by DrawItemSlot.
	// quantity == 0 means the slot is empty.
	// rarityColor.a == 0 means no rarity tint (use the default slot border instead).
	struct UiItemSlotComponent
	{
		std::uint32_t textureSlot = 0; // 0 = no icon texture
		int quantity = 0;              // 0 = empty
		bool selected = false;
		glm::vec4 rarityColor{0.f, 0.f, 0.f, 0.f};
	};

	// -- Label row --------------------------------------------------------------
	// A two-column row: static label on the left, dynamic value on the right.
	struct UiLabelRowComponent
	{
		std::string label;
		std::string value;
		glm::vec4 valueColor{1.f, 1.f, 1.f, 1.f};
		float valueColumnOffsetPx = 206.f;
		float labelMaxWidthPx = 0.f; // 0 = auto-fit to value column / row width
		float valueMaxWidthPx = 0.f; // 0 = auto-fit to remaining row width
		bool truncateLabel = true;
		bool truncateValue = true;
	};

	// -- Selectable row ---------------------------------------------------------
	// Generic clickable row for editor/debug list UIs.
	struct UiSelectableComponent
	{
		std::string label;
		Entity payload{};
		bool selected = false;
		bool visible = true;
		float labelMaxWidthPx = 0.f; // 0 = auto-fit to row width
	};

	// -- Tree node --------------------------------------------------------------
	// Selectable row with indentation and optional expand/collapse affordance.
	struct UiTreeNodeComponent
	{
		std::string label;
		Entity payload{};
		std::uint32_t depth = 0;
		bool selected = false;
		bool expanded = true;
		bool hasChildren = false;
		bool visible = true;
		float indentPx = 14.f;
		float labelMaxWidthPx = 0.f; // 0 = auto-fit to remaining row width
	};

	// -- Section separator ------------------------------------------------------
	// DrawSection renders a horizontal separator line for this entity.
	struct UiSectionComponent
	{
		bool _present = false; // dummy field - some EnTT versions return void from emplace for empty types
	};

	// -- Graph ------------------------------------------------------------------
	// Time-series bar graph widget.  Renders a background rect, proportional bars,
	// reference lines (e.g. 60/30 fps), and a legend label below.
	struct UiGraphComponent
	{
		static constexpr std::size_t kMaxSamples = 128;

		std::array<float, kMaxSamples> samples{};
		std::size_t head = 0;
		std::size_t count = 0;
		float rangeMin = 0.f;
		float rangeMax = 33.333f; // 30 fps

		std::string label;
	};

	// -- Hierarchy --------------------------------------------------------------
	struct UiChildrenComponent
	{
		std::vector<Entity> children;
	};

	// -- Tab bar ---------------------------------------------------------------
	// Horizontal tab bar. Each tab is a button entity in the children list.
	// selectedTab = index into children that is currently active.
	// tabPages = one page entity per tab; DrawTabBar shows/hides them automatically.
	struct UiTabComponent
	{
		std::vector<std::string> tabNames;
		std::size_t selectedTab = 0;
		std::size_t appliedSelectedTab = static_cast<std::size_t>(-1);
		std::vector<Entity> tabPages;
	};

	struct UiParentComponent
	{
		Entity parent;
	};

} // namespace aether::ui
