#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "platform/Input.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiTextBoxSystem.hpp"

using namespace aether;

namespace
{
	// `fonts` is null throughout: the system tolerates it (click-to-caret and scroll-follow are
	// measurement and go quiet), so none of this needs a window, a GPU or a font atlas.
	Entity MakeTextBox(World& w, glm::vec4 rect = {0.f, 0.f, 200.f, 24.f})
	{
		const Entity e = w.Create();
		auto& r = w.Emplace<ui::UIRect>(e);
		r.resolvedRect = rect;
		w.Emplace<ui::UISelectable>(e);
		w.Emplace<ui::UITextBox>(e);
		return e;
	}

	// World exposes TryGet, not Get - these entities always have the component.
	ui::UITextBox& Box(World& w, Entity e)
	{
		return *w.TryGet<ui::UITextBox>(e);
	}

	ui::UISelectable& Sel(World& w, Entity e)
	{
		return *w.TryGet<ui::UISelectable>(e);
	}

	// A fresh Input per tick. Headless there is no Update() to roll prev->curr, so a key set once
	// stays on its down-edge forever; scoping the Input to one frame is how a test gets one edge.
	void Tick(World& w, Input& input, float time)
	{
		ui::UiTextBoxSystem::Update(w, input, nullptr, time);
	}

	void TickIdle(World& w, float time)
	{
		Input idle;
		Tick(w, idle, time);
	}
} // namespace

TEST_CASE("Enter activation enters editing without submitting")
{
	World w;

	const Entity e = MakeTextBox(w);
	Box(w, e).text = "hello";
	Sel(w, e).focused = true;
	Sel(w, e).activated = true; // what UiNavigationSystem does on the Enter down-edge

	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::Enter), true); // ...and the edge is still pressed here
	Tick(w, input, 0.f);

	CHECK(Box(w, e).editing);
	CHECK(w.Has<ui::UIKeyboardCapture>(e));
	CHECK_FALSE(Box(w, e).submitted); // the activation key is not a commit
	CHECK_FALSE(Box(w, e).cancelled);
	CHECK(Box(w, e).text == "hello");
}

TEST_CASE("Space activation does not type a space over the field")
{
	World w;

	const Entity e = MakeTextBox(w);
	Box(w, e).text = "hello";
	Sel(w, e).focused = true;
	Sel(w, e).activated = true; // nav activates on Space too

	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::Space), true);
	input.SetSyntheticChars(" "); // GLFW's char callback fires for Space on the same frame

	Tick(w, input, 0.f);

	CHECK(Box(w, e).editing);
	CHECK(Box(w, e).text == "hello"); // entry selects all, so an unguarded insert would wipe it
	CHECK_FALSE(Box(w, e).changed);
}

TEST_CASE("Losing focus while editing releases the keyboard")
{
	World w;

	const Entity e = MakeTextBox(w);
	Sel(w, e).focused = true;
	Sel(w, e).activated = true;
	TickIdle(w, 0.f);
	REQUIRE(Box(w, e).editing);
	REQUIRE(w.Has<ui::UIKeyboardCapture>(e));

	Sel(w, e).activated = false; // nav rewrites this every frame
	Sel(w, e).focused = false;
	TickIdle(w, 0.016f);

	CHECK_FALSE(Box(w, e).editing);
	CHECK_FALSE(w.Has<ui::UIKeyboardCapture>(e));
}

TEST_CASE("Escape reverts to the committed text and releases the keyboard")
{
	World w;

	const Entity e = MakeTextBox(w);
	Box(w, e).text = "abc";
	Sel(w, e).focused = true;
	Sel(w, e).activated = true;
	TickIdle(w, 0.f);
	REQUIRE(Box(w, e).editing);
	REQUIRE(Box(w, e).committedText == "abc");

	Sel(w, e).activated = false;
	Box(w, e).text = "abcXYZ"; // stand-in for the user having typed

	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::Escape), true);
	Tick(w, input, 0.016f);

	CHECK(Box(w, e).text == "abc");
	CHECK(Box(w, e).cancelled);
	CHECK_FALSE(Box(w, e).submitted);
	CHECK_FALSE(Box(w, e).editing);
	CHECK_FALSE(w.Has<ui::UIKeyboardCapture>(e));
}

TEST_CASE("Enter while already editing submits and releases the keyboard")
{
	World w;

	const Entity e = MakeTextBox(w);
	Box(w, e).text = "abc";
	Sel(w, e).focused = true;
	Sel(w, e).activated = true;
	TickIdle(w, 0.f);
	REQUIRE(Box(w, e).editing);

	Sel(w, e).activated = false;

	Input input;
	input.SetSyntheticKey(static_cast<int>(Key::Enter), true);
	Tick(w, input, 0.016f);

	CHECK(Box(w, e).submitted);
	CHECK_FALSE(Box(w, e).cancelled);
	CHECK_FALSE(Box(w, e).editing);
	CHECK_FALSE(w.Has<ui::UIKeyboardCapture>(e));
	CHECK(Box(w, e).committedText == "abc");
}

TEST_CASE("A disabled ancestor drops editing, dragging and the keyboard")
{
	World w;

	const Entity parent = w.Create();
	w.Emplace<DisabledComponent>(parent);

	const Entity e = MakeTextBox(w);
	w.Emplace<HierarchyComponent>(e).parent = parent;

	Box(w, e).editing = true;
	Box(w, e).dragging = true;
	w.Emplace<ui::UIKeyboardCapture>(e);
	Sel(w, e).focused = true;

	TickIdle(w, 0.f);

	CHECK_FALSE(Box(w, e).editing);
	CHECK_FALSE(Box(w, e).dragging);
	CHECK_FALSE(w.Has<ui::UIKeyboardCapture>(e));
}

TEST_CASE("The changed/submitted/cancelled pulses last exactly one frame")
{
	World w;

	const Entity e = MakeTextBox(w);
	Box(w, e).text = "abc";
	Sel(w, e).focused = true;
	Sel(w, e).activated = true;
	TickIdle(w, 0.f);

	Sel(w, e).activated = false;
	Box(w, e).changed = true; // whatever raised them last frame
	Box(w, e).submitted = true;
	Box(w, e).cancelled = true;

	TickIdle(w, 0.016f);

	CHECK(Box(w, e).editing); // an input-free frame does not end editing
	CHECK_FALSE(Box(w, e).changed);
	CHECK_FALSE(Box(w, e).submitted);
	CHECK_FALSE(Box(w, e).cancelled);
}

TEST_CASE("A stranded keyboard capture is swept")
{
	World w;

	// Nothing here is editing: a marker left behind by an entity that stopped matching the
	// text-box view, or by an outside writer clearing `editing`, would otherwise wedge the
	// keyboard for the rest of the scene's life.
	const Entity orphan = w.Create();
	w.Emplace<ui::UIKeyboardCapture>(orphan);

	const Entity idle = MakeTextBox(w);
	w.Emplace<ui::UIKeyboardCapture>(idle);

	TickIdle(w, 0.f);

	CHECK_FALSE(w.Has<ui::UIKeyboardCapture>(orphan));
	CHECK_FALSE(w.Has<ui::UIKeyboardCapture>(idle));
}

TEST_CASE("Copy and cut leave a password field's plaintext off the clipboard")
{
	World w;

	const Entity e = MakeTextBox(w);
	Box(w, e).text = "hunter2";
	Box(w, e).password = true;
	Sel(w, e).focused = true;
	Sel(w, e).activated = true;
	TickIdle(w, 0.f); // entry selects all
	REQUIRE(Box(w, e).editing);

	Sel(w, e).activated = false;

	Input input;
	input.SetClipboardText("sentinel");
	input.SetSyntheticKey(static_cast<int>(Key::LeftCtrl), true);
	input.SetSyntheticKey(static_cast<int>(Key::X), true);
	Tick(w, input, 0.016f);

	CHECK(input.GetClipboardText() == "sentinel"); // never published
	CHECK(Box(w, e).text.empty());                 // but cut still deletes
}
