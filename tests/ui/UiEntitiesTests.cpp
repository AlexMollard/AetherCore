#include <doctest/doctest.h>

#include <algorithm>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEntities.hpp"

using namespace aether;

TEST_CASE("UI entity factories create named components and hierarchy links")
{
	World world;

	const Entity canvas = ui::CreateCanvasEntity(world);
	REQUIRE(canvas.IsValid());
	CHECK(world.Get<NameComponent>(canvas).name == "Canvas");
	CHECK(world.Has<ui::UICanvas>(canvas));
	CHECK(world.Has<ui::UIRect>(canvas));
	CHECK(world.Has<HierarchyComponent>(canvas));

	const auto& canvasRect = world.Get<ui::UIRect>(canvas);
	CHECK(canvasRect.anchorMin == glm::vec2{0.f, 0.f});
	CHECK(canvasRect.anchorMax == glm::vec2{1.f, 1.f});

	const Entity image = ui::CreateImageEntity(world, canvas);
	REQUIRE(image.IsValid());
	CHECK(world.Get<NameComponent>(image).name == "Image");
	CHECK(world.Has<ui::UIRect>(image));
	CHECK(world.Has<ui::UIImage>(image));
	CHECK(world.Get<HierarchyComponent>(image).parent == canvas);

	const auto& canvasChildren = world.Get<HierarchyComponent>(canvas).children;
	CHECK(std::find(canvasChildren.begin(), canvasChildren.end(), image) != canvasChildren.end());

	const Entity text = ui::CreateTextEntity(world, Entity{});
	REQUIRE(text.IsValid());
	CHECK(world.Get<NameComponent>(text).name == "Text");
	CHECK(world.Has<ui::UIRect>(text));
	CHECK(world.Has<ui::UIText>(text));

	const Entity autoCanvas = world.Get<HierarchyComponent>(text).parent;
	REQUIRE(autoCanvas.IsValid());
	CHECK(world.Has<ui::UICanvas>(autoCanvas));
	CHECK(world.Get<NameComponent>(autoCanvas).name == "Canvas");
}
