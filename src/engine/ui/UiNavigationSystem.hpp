#pragma once

namespace aether
{
	class World;
	class Input;
} // namespace aether

namespace aether::ui
{
	// Drives focus and activation over UISelectable elements: spatial navigation (the nearest
	// selectable in the pressed direction) from the arrow keys, the d-pad or the left stick,
	// plus mouse hover-to-focus and click-to-activate, and activation on Enter, Space or the
	// gamepad's A button. Runs once per frame before scripts, over the active (in-hierarchy)
	// selectables, using the resolved rects from the previous layout pass. Scripts read the
	// result via Ui.IsFocused / Ui.WasActivated.
	//
	// Controller support lives here rather than in any game because it is the same work every
	// time: a screen that already navigates by arrow key navigates by pad with no change to
	// it at all, and the activating key or button is consumed so it cannot also reach the
	// game underneath.
	namespace UiNavigationSystem
	{
		void Update(World& world, Input& input);
	}
} // namespace aether::ui
