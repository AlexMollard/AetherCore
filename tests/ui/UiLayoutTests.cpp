#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayoutSystem.hpp"

using namespace aether::ui;

TEST_CASE("ResolveRect centers a fixed-anchor element") {
    const glm::vec4 parent{0, 0, 1000, 800}; // x,y,w,h
    UIRect r;
    r.anchorMin = r.anchorMax = {0.5f, 0.5f};
    r.offsetMin = {-50, -25};
    r.offsetMax = {50, 25};
    const glm::vec4 out = ResolveRect(parent, r); // x,y,w,h
    CHECK(out.z == doctest::Approx(100));
    CHECK(out.w == doctest::Approx(50));
    CHECK(out.x == doctest::Approx(450));
    CHECK(out.y == doctest::Approx(375));
}

TEST_CASE("ResolveRect stretches with split anchors") {
    const glm::vec4 parent{0, 0, 1000, 800};
    UIRect r;
    r.anchorMin = {0.0f, 0.0f};
    r.anchorMax = {1.0f, 1.0f};
    r.offsetMin = {10, 20};
    r.offsetMax = {-10, -20};
    const glm::vec4 out = ResolveRect(parent, r);
    CHECK(out.x == doctest::Approx(10));
    CHECK(out.y == doctest::Approx(20));
    CHECK(out.z == doctest::Approx(980));
    CHECK(out.w == doctest::Approx(760));
}

TEST_CASE("ResolveCanvases propagates resolved rects down a nested hierarchy") {
    using namespace aether;
    World world; // fresh World retires the raw-0 null slot, so the canvas below is a valid parent

    Entity canvas = world.Create();
    world.Emplace<ui::UICanvas>(canvas);
    world.Emplace<ui::UIRect>(canvas);
    world.Emplace<HierarchyComponent>(canvas);

    Entity child = world.Create(); // centered 200x200 in the canvas
    auto& cr = world.Emplace<ui::UIRect>(child);
    cr.anchorMin = cr.anchorMax = {0.5f, 0.5f};
    cr.offsetMin = {-100.f, -100.f};
    cr.offsetMax = {100.f, 100.f};
    world.Emplace<HierarchyComponent>(child);
    REQUIRE(ecs::SetParent(world, child, canvas));

    Entity grandchild = world.Create(); // top-left 50x50 inside child
    auto& gr = world.Emplace<ui::UIRect>(grandchild);
    gr.anchorMin = gr.anchorMax = {0.f, 0.f};
    gr.offsetMin = {10.f, 10.f};
    gr.offsetMax = {60.f, 60.f};
    world.Emplace<HierarchyComponent>(grandchild);
    REQUIRE(ecs::SetParent(world, grandchild, child));

    ui::ResolveCanvases(world, {1000.f, 800.f});

    const auto& canvasRect = world.Get<ui::UIRect>(canvas).resolvedRect;
    CHECK(canvasRect.z == doctest::Approx(1000.f));
    CHECK(canvasRect.w == doctest::Approx(800.f));

    const auto& childRect = world.Get<ui::UIRect>(child).resolvedRect;
    CHECK(childRect.x == doctest::Approx(400.f)); // 500 - 100
    CHECK(childRect.y == doctest::Approx(300.f)); // 400 - 100
    CHECK(childRect.z == doctest::Approx(200.f));
    CHECK(childRect.w == doctest::Approx(200.f));

    const auto& gcRect = world.Get<ui::UIRect>(grandchild).resolvedRect;
    CHECK(gcRect.x == doctest::Approx(410.f)); // child.x(400) + 10
    CHECK(gcRect.y == doctest::Approx(310.f)); // child.y(300) + 10
    CHECK(gcRect.z == doctest::Approx(50.f));
    CHECK(gcRect.w == doctest::Approx(50.f));
}
