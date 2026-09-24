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
    Entity e = world.Create();

    MaterialSystem::SetMetallic(world, e, reg, cache, 0.0f);
    // Even though 0.0 matches the seeded default, a freshly created instance must
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
    Entity e = world.Create();

    MaterialSystem::SetRoughness(world, e, reg, cache, 0.3f);
    const int allocs = sink.allocCount;
    const int writes = sink.writeCount;

    MaterialSystem::SetRoughness(world, e, reg, cache, 0.3f);
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
    Entity e = world.Create();

    MaterialSystem::SetMetallic(world, e, reg, cache, 0.2f);
    const std::uint32_t firstSlot = world.Get<MaterialComponent>(e).gpuSlot;

    MaterialSystem::SetMetallic(world, e, reg, cache, 0.9f);
    CHECK(world.Get<MaterialComponent>(e).gpuSlot != firstSlot);
    CHECK(sink.allocCount == 2);

    CHECK(world.Get<MaterialInstanceComponent>(e).asset.metallicFactor == doctest::Approx(0.9f));
}

TEST_CASE("Edits accumulate on the same instance, not a fresh default each time") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    Entity e = world.Create();

    MaterialSystem::SetBaseColor(world, e, reg, cache, glm::vec3(1, 0, 0));
    MaterialSystem::SetMetallic(world, e, reg, cache, 0.5f);

    const MaterialAsset& a = world.Get<MaterialInstanceComponent>(e).asset;
    CHECK(a.baseColorFactor.r == doctest::Approx(1.0f));
    CHECK(a.metallicFactor == doctest::Approx(0.5f));
}

TEST_CASE("GetEmissive on an entity with no material component defaults to black") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    World world;
    Entity e = world.Create();

    CHECK(MaterialSystem::GetEmissive(world, e, reg) == glm::vec3(0.0f));
    // A plain read must never have the side effect a setter's seed-on-first-write
    // does - see MaterialSystem::GetEmissive's own header comment on why: the scene
    // serializer's CaptureMaterial treats the mere PRESENCE of a MaterialInstanceComponent
    // as "this entity has a real per-entity override to save", so a getter that created
    // one just from being queried (e.g. by a highlight helper checking a baseline before
    // ever tinting anything) would make every highlighted-but-never-actually-recoloured
    // entity look permanently overridden to a save.
    CHECK(world.TryGet<MaterialInstanceComponent>(e) == nullptr);
}

TEST_CASE("GetEmissive round-trips exactly what SetEmissive wrote") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    Entity e = world.Create();

    MaterialSystem::SetEmissive(world, e, reg, cache, glm::vec3(0.15f, 0.55f, 0.85f));
    const glm::vec3 read = MaterialSystem::GetEmissive(world, e, reg);
    CHECK(read.r == doctest::Approx(0.15f));
    CHECK(read.g == doctest::Approx(0.55f));
    CHECK(read.b == doctest::Approx(0.85f));

    // Restoring exactly what GetEmissive reported before any tint - the actual
    // highlight-restore contract a caller like Highlight.Clear depends on - must land
    // back at the true original, not an assumed black.
    MaterialSystem::SetEmissive(world, e, reg, cache, glm::vec3(1.0f, 0.0f, 0.0f));
    MaterialSystem::SetEmissive(world, e, reg, cache, read);
    const glm::vec3 restored = MaterialSystem::GetEmissive(world, e, reg);
    CHECK(restored.r == doctest::Approx(0.15f));
    CHECK(restored.g == doctest::Approx(0.55f));
    CHECK(restored.b == doctest::Approx(0.85f));
}
