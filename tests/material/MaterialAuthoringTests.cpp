#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialAuthoring.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "FakeSlotSink.hpp"
#include "FakePipelineFactory.hpp"
#include "FakeTextureSink.hpp"

using namespace aether;

namespace
{
	Entity MakeEntity(World& world)
	{
		return world.Create();
	}
}

TEST_CASE("Many entities bound to one unedited material share a single slot") {
    FakeSlotSink sink(16);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    MaterialAuthoring authoring(reg, cache);
    World world;

    MaterialAsset seed; seed.baseColorFactor = {0.2f, 0.6f, 0.9f, 1.0f};
    const std::uint32_t id = authoring.Create(seed);

    for (int i = 0; i < 5; ++i)
    {
        authoring.Bind(world, MakeEntity(world), id);
    }
    CHECK(sink.allocCount == 1);
}

TEST_CASE("Editing a material re-binds every bound entity to the new content") {
    FakeSlotSink sink(16);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    MaterialAuthoring authoring(reg, cache);
    World world;

    MaterialAsset seed; seed.baseColorFactor = {1, 0, 0, 1};
    const std::uint32_t id = authoring.Create(seed);

    Entity a = MakeEntity(world);
    Entity b = MakeEntity(world);
    authoring.Bind(world, a, id);
    authoring.Bind(world, b, id);
    const std::uint32_t sharedSlot = world.Get<MaterialComponent>(a).gpuSlot;
    CHECK(world.Get<MaterialComponent>(b).gpuSlot == sharedSlot);

    authoring.SetMetallic(world, id, 0.8f);
    const std::uint32_t newSlot = world.Get<MaterialComponent>(a).gpuSlot;
    CHECK(newSlot != sharedSlot);
    CHECK(world.Get<MaterialComponent>(b).gpuSlot == newSlot);
}

TEST_CASE("Two ids with identical seeds dedup, then diverge on edit") {
    FakeSlotSink sink(16);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    MaterialAuthoring authoring(reg, cache);
    World world;

    MaterialAsset seed; seed.baseColorFactor = {0.5f, 0.5f, 0.5f, 1};
    const std::uint32_t idA = authoring.Create(seed);
    const std::uint32_t idB = authoring.Create(seed);

    Entity ea = MakeEntity(world);
    Entity eb = MakeEntity(world);
    authoring.Bind(world, ea, idA);
    authoring.Bind(world, eb, idB);
    CHECK(sink.allocCount == 1);

    authoring.SetRoughness(world, idB, 0.1f);
    CHECK(world.Get<MaterialComponent>(ea).gpuSlot != world.Get<MaterialComponent>(eb).gpuSlot);
    CHECK(sink.allocCount == 2);
}

TEST_CASE("No-op edit on a material does not churn slots") {
    FakeSlotSink sink(16);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    MaterialAuthoring authoring(reg, cache);
    World world;

    MaterialAsset seed; seed.metallicFactor = 0.4f;
    const std::uint32_t id = authoring.Create(seed);
    authoring.Bind(world, MakeEntity(world), id);
    const int allocs = sink.allocCount;

    authoring.SetMetallic(world, id, 0.4f);
    CHECK(sink.allocCount == allocs);
    CHECK(sink.freeCount == 0);
}

TEST_CASE("Re-binding an entity untracks it from the previous material") {
    FakeSlotSink sink(16);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    MaterialAuthoring authoring(reg, cache);
    World world;

    MaterialAsset red; red.baseColorFactor = {1, 0, 0, 1};
    MaterialAsset blue; blue.baseColorFactor = {0, 0, 1, 1};
    const std::uint32_t idRed = authoring.Create(red);
    const std::uint32_t idBlue = authoring.Create(blue);

    Entity e = MakeEntity(world);
    authoring.Bind(world, e, idRed);
    authoring.Bind(world, e, idBlue);
    const std::uint32_t blueSlot = world.Get<MaterialComponent>(e).gpuSlot;

    // Editing the RED material (which e is no longer bound to) must NOT touch e.
    authoring.SetMetallic(world, idRed, 0.9f);
    CHECK(world.Get<MaterialComponent>(e).gpuSlot == blueSlot);
}

TEST_CASE("ReleaseAll drops bookkeeping without touching the sink") {
    FakeSlotSink sink(16);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    MaterialAuthoring authoring(reg, cache);
    World world;

    MaterialAsset seed;
    const std::uint32_t id = authoring.Create(seed);
    authoring.Bind(world, MakeEntity(world), id);
    CHECK(authoring.Count() == 1);

    const int freesBefore = sink.freeCount;
    authoring.ReleaseAll();
    CHECK(authoring.Count() == 0);
    CHECK(sink.freeCount == freesBefore);
}
