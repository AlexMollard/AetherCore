#include <doctest/doctest.h>

#include <cstddef>

#include <entt/entt.hpp>

#include "scene/Entity.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
    // Counts entities that scene capture would actually see: live handles in the
    // entity storage that pass reg.valid(), mirroring CaptureScene's own filter.
    std::size_t LiveEntityCount(World& world)
    {
        auto& reg = world.GetRegistry();
        std::size_t count = 0;
        for (const auto handle: reg.storage<entt::entity>())
        {
            if (reg.valid(handle))
            {
                ++count;
            }
        }
        return count;
    }
} // namespace

// Locks the World construction guarantee that fixes the entt-id-0 / null-entity
// collision: a brand-new World must hand out a VALID first entity, so every
// IsValid()-gated path (ecs::SetParent's child linking, World::Destroy, scene
// capture's validity filter) works from the very first Create() with NO
// "burn entity 0" workaround at the call site.
TEST_CASE("World::Create() on a fresh World returns a valid entity") {
    World world;
    const Entity first = world.Create();
    CHECK(first.IsValid());
}

// The retired null slot must never leak into iteration or capture: a fresh World
// has zero live entities, and after one Create() exactly one shows up (no phantom
// placeholder from the constructor's create+destroy).
TEST_CASE("Fresh World has no phantom entity from the reserved null slot") {
    World world;
    CHECK(LiveEntityCount(world) == 0);

    const Entity first = world.Create();
    CHECK(first.IsValid());
    CHECK(LiveEntityCount(world) == 1); // just the one we created, no phantom
}
