#pragma once

namespace aether
{
	class World;
	class Input;
} // namespace aether

namespace aether::ui
{
	class FontRegistry;

	// Drives UITextBox editing: focus in/out, typed characters, caret keys, clipboard and mouse
	// selection. All of the actual editing rules live in UiTextEdit as pure functions; this is
	// only the marshalling between the ECS + Input and those. Runs after UiNavigationSystem, so
	// it sees this frame's focus and activation.
	//
	// `fonts` may be null (headless, or before the renderer is up): editing still works, but
	// click-to-caret and scroll-follow are measurement and go quiet until a registry arrives.
	class UiTextBoxSystem
	{
	public:
		static void Update(World& world, Input& input, FontRegistry* fonts, float time);
	};
} // namespace aether::ui
