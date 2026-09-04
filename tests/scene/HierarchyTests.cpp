#include <doctest/doctest.h>

#include <algorithm>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
    bool Alive(World& world, Entity e)
    {
        return world.GetRegistry().valid(World::ToEntt(e));
    }

    bool HasChild(World& world, Entity parent, Entity child)
    {
        const auto* h = world.TryGet<HierarchyComponent>(parent);
        if (!h)
        {
            return false;
        }
        return std::find(h->children.begin(), h->children.end(), child) != h->children.end();
    }

    World MakeWorld()
    {
        return World{};
    }
}

TEST_CASE("SetParent links both sides and emplaces components on demand") {
    World world = MakeWorld();
    Entity parent = world.Create();
    Entity child = world.Create();

    CHECK(ecs::SetParent(world, child, parent));
    CHECK(world.Get<HierarchyComponent>(child).parent == parent);
    CHECK(HasChild(world, parent, child));
}

TEST_CASE("SetParent reparents: child leaves the old parent's list") {
    World world = MakeWorld();
    Entity a = world.Create();
    Entity b = world.Create();
    Entity child = world.Create();

    REQUIRE(ecs::SetParent(world, child, a));
    CHECK(ecs::SetParent(world, child, b));

    CHECK(world.Get<HierarchyComponent>(child).parent == b);
    CHECK(!HasChild(world, a, child));
    CHECK(HasChild(world, b, child));
}

TEST_CASE("SetParent rejects self-parenting and cycles without mutating") {
    World world = MakeWorld();
    Entity a = world.Create();
    Entity b = world.Create();
    Entity c = world.Create();
    REQUIRE(ecs::SetParent(world, b, a));
    REQUIRE(ecs::SetParent(world, c, b));

    CHECK(!ecs::SetParent(world, a, a));
    CHECK(!ecs::SetParent(world, a, c));
    CHECK(!ecs::SetParent(world, b, c));

    CHECK(world.Get<HierarchyComponent>(b).parent == a);
    CHECK(world.Get<HierarchyComponent>(c).parent == b);
    CHECK(!world.TryGet<HierarchyComponent>(a)->parent.IsValid());
    CHECK(HasChild(world, a, b));
    CHECK(HasChild(world, b, c));
}

TEST_CASE("IsAncestor: self, chain, and unrelated") {
    World world = MakeWorld();
    Entity a = world.Create();
    Entity b = world.Create();
    Entity c = world.Create();
    Entity stranger = world.Create();
    REQUIRE(ecs::SetParent(world, b, a));
    REQUIRE(ecs::SetParent(world, c, b));

    CHECK(ecs::IsAncestor(world, c, c));
    CHECK(ecs::IsAncestor(world, c, a));
    CHECK(!ecs::IsAncestor(world, a, c));
    CHECK(!ecs::IsAncestor(world, c, stranger));
}

TEST_CASE("Detach: explicit DetachFromParent and SetParent to null both root the child") {
    World world = MakeWorld();
    Entity parent = world.Create();
    Entity c1 = world.Create();
    Entity c2 = world.Create();
    REQUIRE(ecs::SetParent(world, c1, parent));
    REQUIRE(ecs::SetParent(world, c2, parent));

    ecs::DetachFromParent(world, c1);
    CHECK(!world.Get<HierarchyComponent>(c1).parent.IsValid());
    CHECK(!HasChild(world, parent, c1));
    CHECK(HasChild(world, parent, c2));

    CHECK(ecs::SetParent(world, c2, Entity{}));
    CHECK(!world.Get<HierarchyComponent>(c2).parent.IsValid());
    CHECK(world.Get<HierarchyComponent>(parent).children.empty());
}

TEST_CASE("DestroyHierarchy destroys the whole subtree and tidies the parent link") {
    World world = MakeWorld();
    Entity root = world.Create();
    Entity mid = world.Create();
    Entity leafA = world.Create();
    Entity leafB = world.Create();
    Entity sibling = world.Create();
    REQUIRE(ecs::SetParent(world, mid, root));
    REQUIRE(ecs::SetParent(world, leafA, mid));
    REQUIRE(ecs::SetParent(world, leafB, mid));
    REQUIRE(ecs::SetParent(world, sibling, root));

    ecs::DestroyHierarchy(world, mid);

    CHECK(!Alive(world, mid));
    CHECK(!Alive(world, leafA));
    CHECK(!Alive(world, leafB));
    CHECK(Alive(world, root));
    CHECK(Alive(world, sibling));
    CHECK(!HasChild(world, root, mid));
    CHECK(HasChild(world, root, sibling));
}

TEST_CASE("DestroyHierarchy on an entity without HierarchyComponent just destroys it") {
    World world = MakeWorld();
    Entity lone = world.Create();

    ecs::DestroyHierarchy(world, lone);
    CHECK(!Alive(world, lone));
}

TEST_CASE("World::Destroy destroys the subtree and leaves no stale parent handles") {
    World world = MakeWorld();
    Entity root = world.Create();
    Entity doomed = world.Create();
    Entity child = world.Create();
    Entity grandchild = world.Create();
    Entity sibling = world.Create();
    REQUIRE(ecs::SetParent(world, doomed, root));
    REQUIRE(ecs::SetParent(world, child, doomed));
    REQUIRE(ecs::SetParent(world, grandchild, child));
    REQUIRE(ecs::SetParent(world, sibling, root));

    world.Destroy(doomed);

    CHECK(!Alive(world, doomed));
    CHECK(!Alive(world, child));
    CHECK(!Alive(world, grandchild));
    CHECK(Alive(world, root));
    CHECK(Alive(world, sibling));
    CHECK(!HasChild(world, root, doomed));
    CHECK(HasChild(world, root, sibling));

    bool staleParent = false;
    world.View<HierarchyComponent>().each([&](const HierarchyComponent& h) {
        staleParent = staleParent || (h.parent.IsValid() && !Alive(world, h.parent));
    });
    CHECK_FALSE(staleParent);

    // The destroyed subtree must not leak into the root list either.
    const auto& roots = world.Roots();
    CHECK(std::find(roots.begin(), roots.end(), doomed) == roots.end());
    CHECK(std::find(roots.begin(), roots.end(), child) == roots.end());
}
