#pragma once

namespace aether
{
	class World;
	class Input;
} // namespace aether

namespace aether::ui
{
	// Drives focus and activation over UISelectable elements: spatial keyboard navigation
	// (the nearest selectable in the pressed arrow direction) plus mouse hover-to-focus and
	// click-to-activate. Runs once per frame before scripts, over the active (in-hierarchy)
	// selectables, using the resolved rects from the previous layout pass. Scripts read the
	// result via Ui.IsFocused / Ui.WasActivated.
	namespace UiNavigationSystem
	{
		void Update(World& world, Input& input);
	}
} // namespace aether::ui
