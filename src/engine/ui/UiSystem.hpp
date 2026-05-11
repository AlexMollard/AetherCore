#pragma once

#include "vulkan/volk.hpp"

namespace aether
{
	class Input;
}

namespace aether::ui
{
	class UiWorld;
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
	// EndFrame: reserved for post-gui cleanup (e.g. tooltip timers).
	class UiSystem
	{
	public:
		void BeginFrame(UiWorld& world, const Input& input, UiContext& ctx, VkExtent2D extent);
		void EndFrame(UiWorld& world, UiContext& ctx);

	private:
		void HitTest(UiWorld& world, UiContext& ctx, VkExtent2D extent);
		void UpdateDrag(UiWorld& world, UiContext& ctx, VkExtent2D extent);
		void FlushWidgetStates(UiWorld& world, UiContext& ctx);
	};

} // namespace aether::ui
