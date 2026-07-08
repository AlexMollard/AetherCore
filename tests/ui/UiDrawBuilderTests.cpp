#include <doctest/doctest.h>

#include <glm/glm.hpp>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiDrawBuilder.hpp"

using namespace aether;

static ui::FontAsset MakeMonoFont()
{
	ui::FontAsset f;
	f.atlasBindlessSlot = 42;
	f.atlasWidth = f.atlasHeight = 128;
	f.ascent = 40;
	f.descent = 10;
	f.lineHeight = 50;
	f.bakeSize = 48;
	f.glyphs['A'] = ui::GlyphMeta{'A', 0.f, 0.f, 0.1f, 0.1f, 20.f, 30.f, 0.f, 30.f, 24.f};
	f.glyphs['B'] = ui::GlyphMeta{'B', 0.1f, 0.f, 0.2f, 0.1f, 20.f, 30.f, 0.f, 30.f, 24.f};
	return f;
}

TEST_CASE("Builder emits a rect command for a UIImage child, none for the canvas")
{
	World w; // fresh World retires the raw-0 null slot, so the canvas below is a valid parent

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity panel = w.Create();
	auto& pr = w.Emplace<ui::UIRect>(panel);
	pr.resolvedRect = {100, 100, 200, 50};
	auto& pi = w.Emplace<ui::UIImage>(panel);
	pi.color = {1, 0, 0, 1};
	w.Emplace<HierarchyComponent>(panel);
	ecs::SetParent(w, panel, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1); // canvas has no UIImage; only the panel emits
	CHECK(cmds[0].type == ui::kShapeRect);
	CHECK(cmds[0].data0.x == doctest::Approx(100));
	CHECK(cmds[0].data0.z == doctest::Approx(200));
	CHECK(cmds[0].color.r == doctest::Approx(1));
	CHECK(cmds[0].layer == 0);
}

TEST_CASE("Builder emits glyph commands for UIText")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity label = w.Create();
	auto& lr = w.Emplace<ui::UIRect>(label);
	lr.resolvedRect = {0, 0, 500, 100};
	auto& lt = w.Emplace<ui::UIText>(label);
	lt.text = "AB";
	lt.fontName = "mono";
	lt.pixelSize = 48.f;
	lt.color = {0.25f, 0.5f, 0.75f, 1.f};
	w.Emplace<HierarchyComponent>(label);
	ecs::SetParent(w, label, canvas);

	ui::FontRegistry fonts;
	fonts.InjectForTest("mono", MakeMonoFont());

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds, &fonts);

	REQUIRE(cmds.size() == 2);
	CHECK(cmds[0].type == ui::kShapeSdfGlyph);
	CHECK(cmds[0].data0.x == doctest::Approx(0.f));
	CHECK(cmds[0].data0.y == doctest::Approx(10.f));
	CHECK(cmds[0].data1.z == doctest::Approx(0.1f));
	CHECK(cmds[0].color.g == doctest::Approx(0.5f));
	CHECK(cmds[0].textureSlot == 42);
	CHECK(cmds[0].layer == 0);
	CHECK(cmds[1].type == ui::kShapeSdfGlyph);
	CHECK(cmds[1].data0.x == doctest::Approx(24.f));
	CHECK(cmds[1].textureSlot == 42);
	CHECK(cmds[1].layer == 1);
}

TEST_CASE("Builder emits image before glyphs on the same entity")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity label = w.Create();
	auto& lr = w.Emplace<ui::UIRect>(label);
	lr.resolvedRect = {0, 0, 500, 100};
	w.Emplace<ui::UIImage>(label);
	auto& lt = w.Emplace<ui::UIText>(label);
	lt.text = "A";
	lt.fontName = "mono";
	lt.pixelSize = 48.f;
	w.Emplace<HierarchyComponent>(label);
	ecs::SetParent(w, label, canvas);

	ui::FontRegistry fonts;
	fonts.InjectForTest("mono", MakeMonoFont());

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds, &fonts);

	REQUIRE(cmds.size() == 2);
	CHECK(cmds[0].type == ui::kShapeRect);
	CHECK(cmds[0].layer == 0);
	CHECK(cmds[1].type == ui::kShapeSdfGlyph);
	CHECK(cmds[1].layer == 1);
}

TEST_CASE("Builder emits parent image before child image")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity parent = w.Create();
	auto& parentRect = w.Emplace<ui::UIRect>(parent);
	parentRect.resolvedRect = {0, 0, 300, 300};
	auto& parentImage = w.Emplace<ui::UIImage>(parent);
	parentImage.color = {1, 0, 0, 1};
	w.Emplace<HierarchyComponent>(parent);
	ecs::SetParent(w, parent, canvas);

	Entity child = w.Create();
	auto& childRect = w.Emplace<ui::UIRect>(child);
	childRect.resolvedRect = {25, 25, 100, 100};
	auto& childImage = w.Emplace<ui::UIImage>(child);
	childImage.color = {0, 1, 0, 1};
	w.Emplace<HierarchyComponent>(child);
	ecs::SetParent(w, child, parent);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 2);
	CHECK(cmds[0].color.r == doctest::Approx(1.f));
	CHECK(cmds[0].color.g == doctest::Approx(0.f));
	CHECK(cmds[0].layer == 0);
	CHECK(cmds[1].color.r == doctest::Approx(0.f));
	CHECK(cmds[1].color.g == doctest::Approx(1.f));
	CHECK(cmds[1].layer == 1);
}
