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
	World w;

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

	REQUIRE(cmds.size() == 1);
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

TEST_CASE("Builder emits track, fill, and handle for a UISlider")
{
	World w;
	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity s = w.Create();
	auto& sr = w.Emplace<ui::UIRect>(s);
	sr.resolvedRect = {100, 100, 200, 20};
	auto& slider = w.Emplace<ui::UISlider>(s);
	slider.minValue = 0.f;
	slider.maxValue = 1.f;
	slider.value = 0.5f;
	slider.handleRadius = 10.f;
	w.Emplace<HierarchyComponent>(s);
	ecs::SetParent(w, s, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 3);
	CHECK(cmds[0].type == ui::kShapeRect);   // track
	CHECK(cmds[1].type == ui::kShapeRect);   // fill
	CHECK(cmds[2].type == ui::kShapeCircle); // handle
	// inner track x0 = 102, inner width = 196, fill = 98 -> handle centre x = 200
	CHECK(cmds[2].data0.x == doctest::Approx(200.f));
	CHECK(cmds[2].data0.z == doctest::Approx(10.f)); // radius (unfocused)
}

TEST_CASE("Builder emits track and fill for a UIProgressBar")
{
	World w;
	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity p = w.Create();
	auto& pr = w.Emplace<ui::UIRect>(p);
	pr.resolvedRect = {0, 0, 200, 16};
	auto& bar = w.Emplace<ui::UIProgressBar>(p);
	bar.value = 0.25f;
	w.Emplace<HierarchyComponent>(p);
	ecs::SetParent(w, p, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 2);
	CHECK(cmds[0].type == ui::kShapeRect);
	CHECK(cmds[1].type == ui::kShapeRect);
	// inner width 196 * 0.25 = 49
	CHECK(cmds[1].data0.z == doctest::Approx(49.f));
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

TEST_CASE("Draw commands are unclipped by default")
{
	static_assert(sizeof(ui::UiDrawCommand) == 80, "must match DrawCommandData std430 layout");

	const ui::UiDrawCommand cmd;
	CHECK((cmd.flags & ui::kFlagClip) == 0u);
	CHECK(cmd.clipRect.x == doctest::Approx(0.f));
	CHECK(cmd.clipRect.z == doctest::Approx(0.f));
}

TEST_CASE("UIMask clips its subtree to its own rect")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity mask = w.Create();
	auto& mr = w.Emplace<ui::UIRect>(mask);
	mr.resolvedRect = {100, 100, 200, 50};
	w.Emplace<ui::UIMask>(mask);
	w.Emplace<HierarchyComponent>(mask);
	ecs::SetParent(w, mask, canvas);

	Entity child = w.Create();
	auto& chr = w.Emplace<ui::UIRect>(child);
	chr.resolvedRect = {0, 0, 1000, 800}; // deliberately overflows the mask
	w.Emplace<ui::UIImage>(child);
	w.Emplace<HierarchyComponent>(child);
	ecs::SetParent(w, child, mask);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1);
	CHECK((cmds[0].flags & ui::kFlagClip) != 0u);
	CHECK(cmds[0].clipRect.x == doctest::Approx(100));
	CHECK(cmds[0].clipRect.y == doctest::Approx(100));
	CHECK(cmds[0].clipRect.z == doctest::Approx(200));
	CHECK(cmds[0].clipRect.w == doctest::Approx(50));
}

TEST_CASE("Nested UIMasks intersect, and padding shrinks the clip")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity outer = w.Create();
	auto& our = w.Emplace<ui::UIRect>(outer);
	our.resolvedRect = {0, 0, 300, 300};
	w.Emplace<ui::UIMask>(outer);
	w.Emplace<HierarchyComponent>(outer);
	ecs::SetParent(w, outer, canvas);

	Entity inner = w.Create();
	auto& inr = w.Emplace<ui::UIRect>(inner);
	inr.resolvedRect = {100, 100, 400, 400}; // overhangs `outer` to the right and bottom
	auto& innerMask = w.Emplace<ui::UIMask>(inner);
	innerMask.padding = 10.f;
	w.Emplace<ui::UIImage>(inner);
	w.Emplace<HierarchyComponent>(inner);
	ecs::SetParent(w, inner, outer);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1);
	CHECK((cmds[0].flags & ui::kFlagClip) != 0u);
	CHECK(cmds[0].clipRect.x == doctest::Approx(110)); // 100 + 10 padding
	CHECK(cmds[0].clipRect.y == doctest::Approx(110));
	CHECK(cmds[0].clipRect.z == doctest::Approx(190)); // clipped by outer's right edge at 300
	CHECK(cmds[0].clipRect.w == doctest::Approx(190));
}

TEST_CASE("A disabled UIMask does not clip")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity mask = w.Create();
	auto& mr = w.Emplace<ui::UIRect>(mask);
	mr.resolvedRect = {100, 100, 200, 50};
	auto& m = w.Emplace<ui::UIMask>(mask);
	m.enabled = false;
	w.Emplace<ui::UIImage>(mask);
	w.Emplace<HierarchyComponent>(mask);
	ecs::SetParent(w, mask, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1);
	CHECK((cmds[0].flags & ui::kFlagClip) == 0u);
}

TEST_CASE("Text box emits a background and clips its glyphs to the padded inner rect")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 40};
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "AB";
	box.pixelSize = 48.f;
	box.padding = 8.f;
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, cmds, materials, &fonts, nullptr);

	REQUIRE(cmds.size() == 3); // background + two glyphs

	CHECK(cmds[0].type == ui::kShapeRect);
	CHECK((cmds[0].flags & ui::kFlagClip) == 0u); // the box IS the boundary; it is not clipped
	CHECK(cmds[0].data0.z == doctest::Approx(200));

	for (std::size_t i = 1; i < cmds.size(); ++i)
	{
		CHECK(cmds[i].type == ui::kShapeSdfGlyph);
		CHECK((cmds[i].flags & ui::kFlagClip) != 0u);
		CHECK(cmds[i].clipRect.x == doctest::Approx(108)); // 100 + 8 padding
		CHECK(cmds[i].clipRect.z == doctest::Approx(184)); // 200 - 2 * 8
		// Horizontal-only: padding must NOT clip vertically, or a font taller than
		// (height - 2 * padding) gets its ascenders and descenders chopped off.
		CHECK(cmds[i].clipRect.y == doctest::Approx(100)); // the box's own top edge
		CHECK(cmds[i].clipRect.w == doctest::Approx(40));  // the box's full height
	}
}

TEST_CASE("Text box does not chop a font taller than its padded height")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 28}; // the default widget height
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "AB";
	box.pixelSize = 28.f; // taller than 28 - 2 * 8 padding, the case that used to clip
	box.padding = 8.f;
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, cmds, materials, &fonts, nullptr);

	REQUIRE(cmds.size() == 3);
	for (std::size_t i = 1; i < cmds.size(); ++i)
	{
		// The glyph band must fit inside the clip, not overflow it top or bottom.
		CHECK(cmds[i].clipRect.y <= cmds[i].data0.y + 0.001f);
		CHECK(cmds[i].clipRect.y + cmds[i].clipRect.w >= cmds[i].data0.y + cmds[i].data0.w - 0.001f);
	}
}

TEST_CASE("Text box emits a caret only while editing")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 40};
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "AB";
	box.pixelSize = 48.f;
	box.caret = 2;
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> notEditing;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, notEditing, materials, &fonts, nullptr);
	const std::size_t idleCount = notEditing.size();

	box.editing = true;
	box.caretTimer = 0.f; // blink's visible half
	std::vector<ui::UiDrawCommand> editing;
	ui::BuildDrawCommands(w, editing, materials, &fonts, nullptr);

	CHECK(editing.size() == idleCount + 1);
	CHECK(editing.back().type == ui::kShapeRect);
	CHECK(editing.back().data0.x == doctest::Approx(156)); // 108 inner + 2 glyphs * 24 px
}

TEST_CASE("Text box shows the placeholder when empty")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 40};
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "";
	box.placeholder = "AB";
	box.pixelSize = 48.f;
	box.placeholderColor = {0.5f, 0.5f, 0.5f, 1.f};
	// Stale state a script can leave behind by clearing text outside the editing update:
	// neither should reach the placeholder.
	box.scrollX = 90.f;
	box.selectionAnchor = 0;
	box.caret = 2;
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, cmds, materials, &fonts, nullptr);

	REQUIRE(cmds.size() == 3); // background + two glyphs: no selection rect for a placeholder
	CHECK(cmds[1].color.r == doctest::Approx(0.5f)); // drawn in the placeholder colour
	CHECK(cmds[1].data0.x == doctest::Approx(108));  // at the left edge, not shifted by scrollX
}
