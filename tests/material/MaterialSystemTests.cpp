#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "FakeSlotSink.hpp"
#include "FakePipelineFactory.hpp"
#include "FakeTextureSink.hpp"

using namespace aether;

TEST_CASE("AssignMaterial same-content reassign dedups instead of freeing") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;

    Entity e = world.Create();
    MaterialAsset a; a.baseColorFactor = {1,0,0,1};

    MaterialSystem::AssignMaterial(world, e, reg, cache, a);
    CHECK(sink.allocCount == 1);
    CHECK(sink.writeCount == 1);
    const MaterialHandle first = world.Get<MaterialComponent>(e).handle;

    // Re-assign identical content: must be a pure dedup hit - the live slot
    MaterialSystem::AssignMaterial(world, e, reg, cache, a);
    CHECK(sink.freeCount == 0);
    CHECK(sink.allocCount == 1);
    CHECK(sink.writeCount == 1);
    CHECK(world.Get<MaterialComponent>(e).handle == first);
}

TEST_CASE("AssignMaterial changed content acquires before releasing the old slot") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;

    Entity e = world.Create();
    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialSystem::AssignMaterial(world, e, reg, cache, red);
    const std::uint32_t oldSlot = world.Get<MaterialComponent>(e).gpuSlot;

    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};
    MaterialSystem::AssignMaterial(world, e, reg, cache, blue);

    CHECK(world.Get<MaterialComponent>(e).gpuSlot != oldSlot);
    CHECK(sink.allocCount == 2);
    CHECK(sink.freeCount == 1);
}

TEST_CASE("Destroying an entity releases its material handle via the hook") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    MaterialSystem::ConnectLifecycle(world, reg);

    Entity e = world.Create();
    MaterialAsset a; a.baseColorFactor = {0,1,0,1};
    MaterialSystem::AssignMaterial(world, e, reg, cache, a);
    CHECK(sink.freeCount == 0);

    world.Destroy(e);
    CHECK(sink.freeCount == 1);

    MaterialSystem::DisconnectLifecycle(world);
}
