#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "FakeSlotSink.hpp"

using namespace aether;

TEST_CASE("AssignMaterial same-content reassign dedups instead of freeing") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);
    World world;

    Entity e = world.Create();
    MaterialAsset a; a.baseColorFactor = {1,0,0,1};

    MaterialSystem::AssignMaterial(world, e, reg, a);
    CHECK(sink.allocCount == 1);
    CHECK(sink.writeCount == 1);
    const MaterialHandle first = world.Get<MaterialComponent>(e).handle;

    // Re-assign identical content: must be a pure dedup hit - the live slot
    // never drops to refcount zero, so no free, no realloc, no GPU rewrite.
    MaterialSystem::AssignMaterial(world, e, reg, a);
    CHECK(sink.freeCount == 0);
    CHECK(sink.allocCount == 1);
    CHECK(sink.writeCount == 1);
    CHECK(world.Get<MaterialComponent>(e).handle == first);
}

TEST_CASE("AssignMaterial changed content acquires before releasing the old slot") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);
    World world;

    Entity e = world.Create();
    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialSystem::AssignMaterial(world, e, reg, red);
    const std::uint32_t oldSlot = world.Get<MaterialComponent>(e).gpuSlot;

    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};
    MaterialSystem::AssignMaterial(world, e, reg, blue);

    // The new slot was allocated while the old one was still alive, so the
    // just-freed slot's bytes were not rewritten by this reassignment.
    CHECK(world.Get<MaterialComponent>(e).gpuSlot != oldSlot);
    CHECK(sink.allocCount == 2);
    CHECK(sink.freeCount == 1);
}

TEST_CASE("Destroying an entity releases its material handle via the hook") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);
    World world;
    MaterialSystem::ConnectLifecycle(world, reg);

    // entt's first entity gets id 0, which Entity::IsValid()/World::Destroy
    // treat as the null entity - burn it so the test entity is destroyable.
    (void) world.Create();
    Entity e = world.Create();
    MaterialAsset a; a.baseColorFactor = {0,1,0,1};
    MaterialSystem::AssignMaterial(world, e, reg, a);
    CHECK(sink.freeCount == 0);

    world.Destroy(e);
    CHECK(sink.freeCount == 1);

    MaterialSystem::DisconnectLifecycle(world);
}
