#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
    World MakeWorld()
    {
        return World{};
    }

    Entity MakeEntityAt(World& world, const glm::vec3& pos, const glm::vec3& eulerDeg = {}, const glm::vec3& scale = glm::vec3(1.0f))
    {
        const Entity e = world.Create();
        world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(pos, eulerDeg, scale)});
        return e;
    }

    void CheckMatApprox(const glm::mat4& actual, const glm::mat4& expected)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                CHECK(actual[c][r] == doctest::Approx(expected[c][r]).epsilon(1e-4));
            }
        }
    }

    glm::mat4 RelativeTo(const glm::mat4& parent, const glm::mat4& child)
    {
        return glm::inverse(parent) * child;
    }
}

TEST_CASE("SetWorldTransform moves the whole subtree, preserving relative offsets") {
    World world = MakeWorld();
    const Entity parent = MakeEntityAt(world, {10.0f, 0.0f, 0.0f});
    const Entity child = MakeEntityAt(world, {12.0f, 1.0f, 0.0f}, {0.0f, 45.0f, 0.0f});
    const Entity grandchild = MakeEntityAt(world, {12.0f, 3.0f, 0.5f}, {}, glm::vec3(2.0f));
    REQUIRE(ecs::SetParent(world, child, parent));
    REQUIRE(ecs::SetParent(world, grandchild, child));

    const glm::mat4 childRel = RelativeTo(world.Get<TransformComponent>(parent).localToWorld, world.Get<TransformComponent>(child).localToWorld);
    const glm::mat4 grandRel = RelativeTo(world.Get<TransformComponent>(child).localToWorld, world.Get<TransformComponent>(grandchild).localToWorld);

    const glm::mat4 target = ComposeTransform({-5.0f, 2.0f, 8.0f}, {0.0f, 90.0f, 0.0f}, glm::vec3(1.5f));
    ecs::SetWorldTransform(world, parent, target);

    const glm::mat4& parentNow = world.Get<TransformComponent>(parent).localToWorld;
    const glm::mat4& childNow = world.Get<TransformComponent>(child).localToWorld;
    const glm::mat4& grandNow = world.Get<TransformComponent>(grandchild).localToWorld;

    CheckMatApprox(parentNow, target);
    CheckMatApprox(RelativeTo(parentNow, childNow), childRel);
    CheckMatApprox(RelativeTo(childNow, grandNow), grandRel);
}

TEST_CASE("SetWorldTransform is a pure translation delta for a translated parent") {
    World world = MakeWorld();
    const Entity parent = MakeEntityAt(world, {0.0f, 0.0f, 0.0f});
    const Entity child = MakeEntityAt(world, {3.0f, 0.0f, 0.0f});
    REQUIRE(ecs::SetParent(world, child, parent));

    glm::mat4 target = world.Get<TransformComponent>(parent).localToWorld;
    target[3] = glm::vec4(0.0f, 5.0f, 0.0f, 1.0f);
    ecs::SetWorldTransform(world, parent, target);

    CheckMatApprox(world.Get<TransformComponent>(child).localToWorld, ComposeTransform({3.0f, 5.0f, 0.0f}, {}, glm::vec3(1.0f)));
}

TEST_CASE("SetWorldTransform without a TransformComponent is a no-op") {
    World world = MakeWorld();
    const Entity parent = world.Create();
    const Entity child = MakeEntityAt(world, {1.0f, 2.0f, 3.0f});
    REQUIRE(ecs::SetParent(world, child, parent));

    const glm::mat4 before = world.Get<TransformComponent>(child).localToWorld;
    ecs::SetWorldTransform(world, parent, ComposeTransform({9.0f, 9.0f, 9.0f}, {}, glm::vec3(1.0f)));
    CheckMatApprox(world.Get<TransformComponent>(child).localToWorld, before);
}

TEST_CASE("SetWorldTransform cascades through a transformless middle link") {
    World world = MakeWorld();
    const Entity parent = MakeEntityAt(world, {0.0f, 0.0f, 0.0f});
    const Entity middle = world.Create();
    const Entity leaf = MakeEntityAt(world, {2.0f, 0.0f, 0.0f});
    REQUIRE(ecs::SetParent(world, middle, parent));
    REQUIRE(ecs::SetParent(world, leaf, middle));

    glm::mat4 target = world.Get<TransformComponent>(parent).localToWorld;
    target[3] = glm::vec4(0.0f, 0.0f, -4.0f, 1.0f);
    ecs::SetWorldTransform(world, parent, target);

    CheckMatApprox(world.Get<TransformComponent>(leaf).localToWorld, ComposeTransform({2.0f, 0.0f, -4.0f}, {}, glm::vec3(1.0f)));
}
