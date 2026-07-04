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

TEST_CASE("First instance setter seeds a default and assigns a material") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    (void) world.Create(); // burn entity id 0 (treated as null)
    Entity e = world.Create();

    MaterialSystem::SetMetallic(world, e, reg, cache, 0.0f); // value equals the default
    // Even though 0.0 matches the seeded default, a freshly created instance must
    // still assign so the entity gains a material.
    CHECK(world.TryGet<MaterialInstanceComponent>(e) != nullptr);
    CHECK(world.TryGet<MaterialComponent>(e) != nullptr);
    CHECK(sink.allocCount == 1);
    CHECK(sink.writeCount == 1);
}

TEST_CASE("No-op instance edit does not re-acquire or churn the slot") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    (void) world.Create();
    Entity e = world.Create();

    MaterialSystem::SetRoughness(world, e, reg, cache, 0.3f);
    const int allocs = sink.allocCount;
    const int writes = sink.writeCount;

    MaterialSystem::SetRoughness(world, e, reg, cache, 0.3f); // same value -> gated out
    CHECK(sink.allocCount == allocs);
    CHECK(sink.writeCount == writes);
    CHECK(sink.freeCount == 0);
}

TEST_CASE("Changed instance field re-acquires a new material") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    (void) world.Create();
    Entity e = world.Create();

    MaterialSystem::SetMetallic(world, e, reg, cache, 0.2f);
    const std::uint32_t firstSlot = world.Get<MaterialComponent>(e).gpuSlot;

    MaterialSystem::SetMetallic(world, e, reg, cache, 0.9f); // different -> new content
    CHECK(world.Get<MaterialComponent>(e).gpuSlot != firstSlot);
    CHECK(sink.allocCount == 2);

    // Instance retains the other fields; only metallic changed.
    CHECK(world.Get<MaterialInstanceComponent>(e).asset.metallicFactor == doctest::Approx(0.9f));
}

TEST_CASE("Edits accumulate on the same instance, not a fresh default each time") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    (void) world.Create();
    Entity e = world.Create();

    MaterialSystem::SetBaseColor(world, e, reg, cache, glm::vec3(1, 0, 0));
    MaterialSystem::SetMetallic(world, e, reg, cache, 0.5f);

    const MaterialAsset& a = world.Get<MaterialInstanceComponent>(e).asset;
    CHECK(a.baseColorFactor.r == doctest::Approx(1.0f));
    CHECK(a.metallicFactor == doctest::Approx(0.5f)); // base color survived the metallic edit
}
