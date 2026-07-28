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
