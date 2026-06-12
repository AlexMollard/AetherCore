#pragma once

#include "gpu/GpuEnums.hpp"

namespace aether
{
	class UIRenderer;
	class Input;
	class World;
} // namespace aether

namespace aether::ui
{
	struct UiContext;

	// Runs once per frame before layers call OnGui().
	//
	// BeginFrame:
	//   1. Captures mouse state from Input.
	//   2. Clears per-frame transient flags on all UiInputComponents.
	//   3. Performs hit-testing to determine the hot entity.
	//   4. Handles panel dragging (updates UiTransformComponent.rect).
	//   5. Flushes hover/press/clicked state onto UiInputComponents.
	//
	// RenderAll: automatically draws all ECS UI entities (panels, buttons, sliders,
	// checkboxes, text inputs, progress bars, item slots). No per-frame manual
	// Draw* calls required - just create the ECS components and they render.
	//
	// EndFrame: reserved for post-gui cleanup (e.g. tooltip timers).
	class UiSystem
	{
	public:
		// deltaTime - seconds since last frame; used for hover/press animation lerp.
		void BeginFrame(aether::World& world, Input& input, UiContext& ctx, gpu::Extent2D extent, float deltaTime = 0.f);
		void RenderAll(aether::World& world, UIRenderer& ui, const Input& input, gpu::Extent2D extent);
		void EndFrame(aether::World& world, UiContext& ctx);

	private:
		void HitTest(aether::World& world, UiContext& ctx, gpu::Extent2D extent);
		void UpdateDrag(aether::World& world, UiContext& ctx, gpu::Extent2D extent);
		void FlushWidgetStates(aether::World& world, UiContext& ctx);
		// Lerps hoverT/pressT on every UiInputComponent toward their target [0..1].
		void UpdateTransitions(aether::World& world, float deltaTime);
		// Feeds keyboard events (typed chars, Backspace, arrows, Enter/Escape) into the
		// focused UiTextInputComponent and drives cursor blink.
		void ProcessTextInput(aether::World& world, UiContext& ctx, const Input& input, float deltaTime);
	};

} // namespace aether::ui
