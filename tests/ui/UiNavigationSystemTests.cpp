#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "platform/Input.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiNavigationSystem.hpp"

using namespace aether;

namespace
{
	Entity MakeSelectable(World& w, glm::vec4 rect)
	{
		const Entity e = w.Create();
		auto& r = w.Emplace<ui::UIRect>(e);
		r.resolvedRect = rect;
		w.Emplace<ui::UISelectable>(e);
		return e;
	}

	// World exposes TryGet, not Get - these entities always have the component.
	ui::UISelectable& Sel(World& w, Entity e)
	{
		return *w.TryGet<ui::UISelectable>(e);
	}
} // namespace

TEST_CASE("A captured focus keeps navigation keys away from the nav system")
{
	World w;
	Input input;

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity bottom = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, top).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(top);

	input.SetSyntheticKey(static_cast<int>(Key::Right), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, top).focused);
	CHECK_FALSE(Sel(w, bottom).focused);
	CHECK_FALSE(Sel(w, top).activated);
}

TEST_CASE("A captured focus is not activated by Enter")
{
	World w;
	Input input;

	const Entity field = MakeSelectable(w, {0, 0, 100, 20});
	Sel(w, field).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(field);

	input.SetSyntheticKey(static_cast<int>(Key::Enter), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, field).activated);
}

TEST_CASE("Tab advances focus in reading order even while captured")
{
	World w;
	Input input;

	const Entity first = MakeSelectable(w, {0, 0, 100, 20});
	const Entity second = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, first).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(first);

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, first).focused);
	CHECK(Sel(w, second).focused);
}

TEST_CASE("Shift+Tab moves focus backwards and wraps")
{
	World w;
	Input input;

	const Entity first = MakeSelectable(w, {0, 0, 100, 20});
	const Entity second = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, first).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	input.SetSyntheticKey(static_cast<int>(Key::LeftShift), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, second).focused); // wrapped from first to last
}

TEST_CASE("With no UIKeyboardCapture anywhere, arrows and Enter navigate normally")
{
	World w;
	Input input;

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity bottom = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, top).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Down), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, top).focused);
	CHECK(Sel(w, bottom).focused);

	input.SetSyntheticKey(static_cast<int>(Key::Down), false);
	input.SetSyntheticKey(static_cast<int>(Key::Enter), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, bottom).activated);
}

TEST_CASE("Nothing is focused until the player reaches for the UI, so Space stays the game's")
{
	World w;
	Input input;

	// A gameplay HUD: a canvas exists, but the player has not touched it. The nav system
	// used to focus the first interactable element here, which armed Enter/Space against
	// it - a chat box on a platformer's HUD turned the jump key into "start typing".
	const Entity first = MakeSelectable(w, {0, 0, 100, 20});
	const Entity second = MakeSelectable(w, {0, 100, 100, 20});

	input.SetSyntheticKey(static_cast<int>(Key::Space), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, first).focused);
	CHECK_FALSE(Sel(w, second).focused);
	CHECK_FALSE(Sel(w, first).activated);
	CHECK_FALSE(Sel(w, second).activated);
}

TEST_CASE("A direction key with nothing focused enters the screen instead of moving")
{
	World w;
	Input input;

	// Laid out so "first in reading order" and "nearest downwards from the origin"
	// disagree: topRight leads the reading order (top row), but a spatial search run
	// from an empty focus would land on lowerLeft instead. Seeding has to be a
	// deliberate branch, not a spatial move that happens to start at (0,0).
	const Entity topRight = MakeSelectable(w, {400, 0, 100, 20});
	const Entity lowerLeft = MakeSelectable(w, {0, 100, 100, 20});

	input.SetSyntheticKey(static_cast<int>(Key::Down), true);
	ui::UiNavigationSystem::Update(w, input);

	// The press establishes focus rather than stepping off a focus nobody chose.
	CHECK(Sel(w, topRight).focused);
	CHECK_FALSE(Sel(w, lowerLeft).focused);
	CHECK_FALSE(Sel(w, topRight).activated);
}

TEST_CASE("Tab with nothing focused lands on the first element in reading order")
{
	World w;
	Input input;

	// Created bottom-first so creation order and reading order disagree.
	const Entity below = MakeSelectable(w, {0, 100, 100, 20});
	const Entity top = MakeSelectable(w, {0, 0, 100, 20});

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, top).focused);
	CHECK_FALSE(Sel(w, below).focused);
}

TEST_CASE("Shift+Tab with nothing focused lands on the last element in reading order")
{
	World w;
	Input input;

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity below = MakeSelectable(w, {0, 100, 100, 20});

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	input.SetSyntheticKey(static_cast<int>(Key::LeftShift), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, below).focused);
	CHECK_FALSE(Sel(w, top).focused);
}

TEST_CASE("Focus that stops being interactable is dropped, not left stuck")
{
	World w;
	Input input;

	const Entity locked = MakeSelectable(w, {0, 0, 100, 20});
	const Entity open = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, locked).focused = true;
	Sel(w, locked).interactable = false; // a script locked it this frame

	input.SetSyntheticKey(static_cast<int>(Key::Enter), true);
	ui::UiNavigationSystem::Update(w, input);

	// Neither still focused on something that cannot be activated, nor silently
	// re-pointed at an element the player never chose.
	CHECK_FALSE(Sel(w, locked).focused);
	CHECK_FALSE(Sel(w, locked).activated);
	CHECK_FALSE(Sel(w, open).focused);
	CHECK_FALSE(Sel(w, open).activated);
}

TEST_CASE("Tab orders by row before column, not by creation order")
{
	World w;
	Input input;

	// Created bottom-first so creation order and reading order disagree: Tab must follow
	// the layout (top row left-to-right, then the row below), not the order entities exist in.
	const Entity below = MakeSelectable(w, {0, 100, 100, 20});
	const Entity topLeft = MakeSelectable(w, {0, 0, 100, 20});
	const Entity topRight = MakeSelectable(w, {200, 4, 100, 20}); // same row as topLeft (4px drift)

	Sel(w, topLeft).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	ui::UiNavigationSystem::Update(w, input);
	CHECK(Sel(w, topRight).focused); // across the top row first

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	ui::UiNavigationSystem::Update(w, input);
	CHECK(Sel(w, below).focused); // then down to the next row
}

TEST_CASE("Activating an element consumes the key that did it")
{
	World w;
	Input input;

	const Entity button = MakeSelectable(w, {0, 0, 100, 20});
	Sel(w, button).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Space), true);
	ui::UiNavigationSystem::Update(w, input);

	REQUIRE(Sel(w, button).activated);
	// The Space that pressed a menu button must not also reach the character controller
	// as a jump, and the Enter that chose a menu item must not also open a chat box.
	CHECK(input.IsKeyConsumed(Key::Space));
	CHECK(input.IsKeyConsumed(Key::Enter));
	CHECK_FALSE(input.IsKeyPressed(Key::Space));
}

TEST_CASE("A key that activates nothing is left for the game")
{
	World w;
	Input input;

	// Focused nothing: the resting state of a screen the player has not reached for.
	MakeSelectable(w, {0, 0, 100, 20});

	input.SetSyntheticKey(static_cast<int>(Key::Space), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(input.IsKeyConsumed(Key::Space));
	CHECK(input.IsKeyPressed(Key::Space)); // still the jump button
}

// ── Mouse ────────────────────────────────────────────────────────────────────
// Hover is a real, useful signal for STYLING - Ui.IsHovered reads it directly,
// independently of any of this - but it must never move KEYBOARD focus on its own:
// see Ui.HasFocus's own remarks ("the engine never focuses anything on its own").
// Only a click, a keyboard direction/Tab, or a gamepad direction/button may move
// `focused`; the cursor position below feeds nothing but the click target.
//
// The click half of this (a click resolves `hovered` into `focused`/`activated`, and
// a click on nothing now clears focus) has NO coverage here: Input::SetSyntheticMouseButton
// has no windowless-seed path the way SetSyntheticKey does (see its own remarks) - it
// only takes effect through Input::Update(), which dereferences the real GLFW window
// unconditionally and is unsafe to call with none. Exercising a synthetic click needs a
// live window and cursor, which this suite does not have.

TEST_CASE("Resting the mouse over a selectable does not focus it")
{
	World w;
	Input input;

	const Entity only = MakeSelectable(w, {0, 0, 100, 20});

	input.SetSyntheticMousePos({50.f, 10.f}); // inside the rect, nothing clicked
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, only).focused);
}

TEST_CASE("Hovering a different selectable does not steal focus from the one already chosen")
{
	World w;
	Input input;

	const Entity chosen = MakeSelectable(w, {0, 0, 100, 20});
	const Entity other = MakeSelectable(w, {0, 100, 100, 20});
	Sel(w, chosen).focused = true;

	input.SetSyntheticMousePos({50.f, 110.f}); // inside `other`'s rect, nothing clicked
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, chosen).focused);
	CHECK_FALSE(Sel(w, other).focused);
}

// ── Gamepad navigation ──────────────────────────────────────────────────────
// The point of putting controller support here rather than in a game: these screens are the
// same ones the arrow keys already drove, and none of them had to change.

namespace
{
	// A pad in slot 0 with nothing pressed. Windowless, so the synthetic setters stand in
	// for the poll - see Input::SetSyntheticGamepadConnected.
	void ConnectPad(Input& input)
	{
		input.SetSyntheticGamepadConnected(0, true);
	}
} // namespace

TEST_CASE("The d-pad moves focus like the arrow keys")
{
	World w;
	Input input;
	ConnectPad(input);

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity bottom = MakeSelectable(w, {0, 100, 100, 20});
	Sel(w, top).focused = true;

	input.SetSyntheticGamepadButton(0, GamepadButton::DpadDown, true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, bottom).focused);
	CHECK_FALSE(Sel(w, top).focused);
}

TEST_CASE("A direction on the pad enters a screen that had no focus")
{
	World w;
	Input input;
	ConnectPad(input);

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity bottom = MakeSelectable(w, {0, 100, 100, 20});

	input.SetSyntheticGamepadButton(0, GamepadButton::DpadDown, true);
	ui::UiNavigationSystem::Update(w, input);

	// Lights the first element rather than stepping off one nobody chose.
	CHECK(Sel(w, top).focused);
	CHECK_FALSE(Sel(w, bottom).focused);
}

TEST_CASE("Holding the stick steps one element, not one per frame")
{
	World w;
	Input input;
	ConnectPad(input);

	const Entity a = MakeSelectable(w, {0, 0, 100, 20});
	const Entity b = MakeSelectable(w, {0, 100, 100, 20});
	const Entity c = MakeSelectable(w, {0, 200, 100, 20});
	Sel(w, a).focused = true;

	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f); // hold down
	ui::UiNavigationSystem::Update(w, input);
	REQUIRE(Sel(w, b).focused);

	// Second frame, stick still held: focus must stay put or a three-item menu is
	// unusable on a controller.
	input.SetSyntheticGamepadAxis(0, GamepadAxis::LeftY, 1.0f);
	ui::UiNavigationSystem::Update(w, input);
	CHECK(Sel(w, b).focused);
	CHECK_FALSE(Sel(w, c).focused);
}

TEST_CASE("A activates the focused element")
{
	World w;
	Input input;
	ConnectPad(input);

	const Entity only = MakeSelectable(w, {0, 0, 100, 20});
	Sel(w, only).focused = true;

	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, only).activated);
}

// The reason the nav system consumes: A is both "confirm" on every menu and "jump" in every
// platformer, so an unconsumed A resumes the game AND jumps on the same frame.
TEST_CASE("The A that activates a menu item never reaches the game")
{
	World w;
	Input input;
	ConnectPad(input);

	const Entity only = MakeSelectable(w, {0, 0, 100, 20});
	Sel(w, only).focused = true;

	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	REQUIRE(input.IsGamepadButtonPressed(GamepadButton::A));

	ui::UiNavigationSystem::Update(w, input);

	REQUIRE(Sel(w, only).activated);
	CHECK_FALSE(input.IsGamepadButtonPressed(GamepadButton::A));
	CHECK_FALSE(input.IsGamepadButtonDown(GamepadButton::A));
}

// With nothing focused there is no menu to confirm, so A belongs to the game and must
// survive - otherwise merely having a canvas on screen would eat the jump button.
TEST_CASE("A is left alone when nothing is focused")
{
	World w;
	Input input;
	ConnectPad(input);

	MakeSelectable(w, {0, 0, 100, 20});

	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(input.IsGamepadButtonPressed(GamepadButton::A));
}

TEST_CASE("A field that owns the keyboard also owns the pad")
{
	World w;
	Input input;
	ConnectPad(input);

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity bottom = MakeSelectable(w, {0, 100, 100, 20});
	Sel(w, top).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(top);

	input.SetSyntheticGamepadButton(0, GamepadButton::DpadDown, true);
	input.SetSyntheticGamepadButton(0, GamepadButton::A, true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, top).focused);
	CHECK_FALSE(Sel(w, bottom).focused);
	CHECK_FALSE(Sel(w, top).activated);
}

// A screen that closes itself on activation - a Resume button hiding the pause menu - empties
// the candidate list on the very next frame. The clear that ends a one-frame activation used
// to live after an early return, so `activated` stayed true until the screen was shown again,
// and then fired immediately. The menu resumed the instant it was opened, every time after
// the first.
TEST_CASE("An activation does not survive the frame its screen disappears")
{
	World w;
	Input input;

	const Entity button = MakeSelectable(w, {0, 0, 100, 20});
	Sel(w, button).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Enter), true);
	ui::UiNavigationSystem::Update(w, input);
	REQUIRE(Sel(w, button).activated);

	// The screen goes away: with no UIRect the element is no longer a navigation candidate,
	// which is the same position a hidden screen leaves it in.
	w.Remove<ui::UIRect>(button);
	input.ClearSyntheticKeys();
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, button).activated);
	// Focus is persistent state and should survive, so the screen comes back where it was.
	CHECK(Sel(w, button).focused);
}

TEST_CASE("Arrow keys stay inside a selectable's group")
{
	// Two stacked pairs. Without groups, Down from the top of the left pair would find
	// whichever centre is nearest below it; with groups it must stay in its own column.
	World w;
	Input input;

	const Entity leftTop = MakeSelectable(w, {0, 0, 100, 20});
	const Entity leftBottom = MakeSelectable(w, {0, 200, 100, 20});
	const Entity rightNear = MakeSelectable(w, {0, 50, 100, 20});

	Sel(w, leftTop).group = "left";
	Sel(w, leftBottom).group = "left";
	Sel(w, rightNear).group = "right";
	Sel(w, leftTop).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Down), true);
	ui::UiNavigationSystem::Update(w, input);

	// rightNear is much closer, so picking leftBottom is only explicable by the group.
	CHECK(Sel(w, leftBottom).focused);
	CHECK_FALSE(Sel(w, rightNear).focused);
}

TEST_CASE("A selectable with no group still reaches everything")
{
	// The compatibility case: content that never set a group must navigate as it always did,
	// including onto selectables that DO carry one.
	World w;
	Input input;

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity below = MakeSelectable(w, {0, 100, 100, 20});
	Sel(w, below).group = "somewhere";
	Sel(w, top).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Down), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, below).focused);
}

TEST_CASE("A group with nowhere to go leaves focus alone")
{
	World w;
	Input input;

	const Entity only = MakeSelectable(w, {0, 0, 100, 20});
	const Entity elsewhere = MakeSelectable(w, {0, 100, 100, 20});
	Sel(w, only).group = "alone";
	Sel(w, elsewhere).group = "other";
	Sel(w, only).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Down), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, only).focused);
	CHECK_FALSE(Sel(w, elsewhere).focused);
}
