#include <doctest/doctest.h>

#include <cstddef>

#include <entt/entt.hpp>

#include "scene/Entity.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
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
}

// collision: a brand-new World must hand out a VALID first entity, so every
TEST_CASE("World::Create() on a fresh World returns a valid entity") {
    World world;
    const Entity first = world.Create();
    CHECK(first.IsValid());
}

// The retired null slot must never leak into iteration or capture: a fresh World
TEST_CASE("Fresh World has no phantom entity from the reserved null slot") {
    World world;
    CHECK(LiveEntityCount(world) == 0);

    const Entity first = world.Create();
    CHECK(first.IsValid());
    CHECK(LiveEntityCount(world) == 1);
}
