#include <doctest/doctest.h>

#include <glm/glm.hpp>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiDrawBuilder.hpp"

using namespace aether;

TEST_CASE("Builder emits a rect command for a UIImage child, none for the canvas") {
    World w;
    // IMPORTANT ECS gotcha: the first entity created is Entity{0}, which IsValid()
    // treats as null, and ecs::SetParent will NOT populate a null parent's children.
    // Burn id 0 first so the canvas is a valid parent (same as HierarchyTests.cpp).
    (void) w.Create();

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
