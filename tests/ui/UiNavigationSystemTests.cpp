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

TEST_CASE("Tab from nothing focused lands on the first control in reading order")
{
	World w;
	Input input;

	const Entity first = MakeSelectable(w, {0, 0, 100, 20});
	MakeSelectable(w, {0, 100, 100, 20});

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, first).focused);
}
