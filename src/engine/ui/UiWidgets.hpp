#pragma once

#include <string_view>
#include "volk.hpp"

#include "Entity.hpp"
#include "UiLayout.hpp"
#include "UiTheme.hpp"

namespace aether
{
	class UIRenderer;
	class Input;
} // namespace aether

namespace aether::ui
{
	class UiWorld;
	struct UiContext;

	// ── Draw functions ────────────────────────────────────────────────────────
	// Each function reads the entity's ECS components, draws via UIRenderer,
	// and returns the relevant state.  Pass `extent` (from AetherCore::GetSwapchainExtent)
	// so pixel-space math (slider knob, checkbox box) works correctly.
	//
	// Entities must have at minimum:
	//   - UiTransformComponent  (position / size)
	//   - UiInputComponent      (hover / press / clicked - written by UiSystem)
	//   - the widget-specific component (UiButtonComponent, etc.)
	//
	// Use the Spawn* helpers below to create fully-configured entities in one call.

	// Button: draws background + centred label, returns true the frame it's clicked.
	bool DrawButton(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme = UiTheme::Default());

	// Slider: draws track / fill / knob.  Handles horizontal drag via Input.
	// Updates UiSliderComponent::value and returns the new value.
	float DrawSlider(UiWorld& world, Entity entity, UIRenderer& ui, const Input& input, VkExtent2D extent, const UiTheme& theme = UiTheme::Default());

	// Checkbox: draws box + tick + inline label, toggles on click, returns current state.
	bool DrawCheckbox(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme = UiTheme::Default());

	// Progress bar: read-only fill rect proportional to UiSliderComponent::value in [min,max].
	void DrawProgressBar(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme = UiTheme::Default());

	// Panel: draws background + header + title.
	// Returns true if the panel body should be rendered (false when collapsed).
	bool DrawPanel(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme = UiTheme::Default());

	// ── Z-order management ────────────────────────────────────────────────────

	// Raises entity to the highest z-order in the world so it renders and
	// hit-tests on top of everything else.
	void BringToFront(UiWorld& world, Entity entity);

	// ── Layout ───────────────────────────────────────────────────────────────

	// Positions all direct children of `container` according to its
	// UiLayoutComponent (VStack / HStack).  Must be called before drawing
	// children so their UiTransformComponent rects are up to date.
	void ApplyLayout(UiWorld& world, Entity container, VkExtent2D extent);

	// Convenience: calls ApplyLayout on every entity that has both
	// UiLayoutComponent and UiChildrenComponent.
	void RunLayouts(UiWorld& world, VkExtent2D extent);

	// ── Spawn helpers ─────────────────────────────────────────────────────────
	// Create a fully-configured widget entity in one call.

	Entity SpawnButton(UiWorld& world, UiRect rect, std::string_view label, float zOrder = 0.f);

	Entity SpawnSlider(UiWorld& world, UiRect rect, float min, float max, float value = 0.f, float zOrder = 0.f);

	Entity SpawnCheckbox(UiWorld& world, UiRect rect, std::string_view label, bool checked = false, float zOrder = 0.f);

	// Progress bar uses UiSliderComponent (value/min/max) but has no UiInputComponent.
	Entity SpawnProgressBar(UiWorld& world, UiRect rect, float min, float max, float value = 0.f, float zOrder = 0.f);

	// Panel entity with UiTransformComponent + UiInputComponent + UiPanelComponent.
	Entity SpawnPanel(UiWorld& world, UiRect rect, std::string_view title, bool draggable = true, bool collapsible = false, float zOrder = 0.f);

} // namespace aether::ui
