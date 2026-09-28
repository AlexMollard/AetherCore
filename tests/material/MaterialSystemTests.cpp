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

TEST_CASE("Soft-cutout scenery foliage writes depth as a cutout and blends only its edge") {
    // The forward pass clears the prepass depth, so a leaf that did not write depth let water,
    // gems and later foliage in the blended group paint over it; a blended edge that did write
    // depth left halos wherever something behind it was drawn later.
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;

    Entity leaf = world.Create();
    MaterialAsset a; a.foliage = true; a.bakedLighting = true; a.alphaMask = true; a.doubleSided = true;
    MaterialSystem::AssignMaterial(world, leaf, reg, cache, a);

    MaterialTemplate cutout = a.templateDesc;
    cutout.cullMode = gpu::CullMode::None;
    cutout.blendEnable = false;
    cutout.depthWriteEnable = true;
    MaterialTemplate edge = cutout;
    edge.shaderVfsPath = "shaders://gltf_mesh_foliage_edge.spv";
    edge.blendEnable = true;
    edge.depthWriteEnable = false;
    const PipelineComponent& p = world.Get<PipelineComponent>(leaf);
    CHECK_FALSE(p.blended);
    CHECK(p.pipeline == cache.Acquire(cutout));
    CHECK(p.edgePipeline == cache.Acquire(edge));

    // Transparent surfaces still blend in one draw and never write depth.
    Entity water = world.Create();
    MaterialAsset w; w.alphaBlend = true;
    MaterialSystem::AssignMaterial(world, water, reg, cache, w);
    MaterialTemplate noWrite = w.templateDesc;
    noWrite.cullMode = gpu::CullMode::Back;
    noWrite.blendEnable = true;
    noWrite.depthWriteEnable = false;
    const PipelineComponent& pw = world.Get<PipelineComponent>(water);
    CHECK(pw.blended);
    CHECK(pw.pipeline == cache.Acquire(noWrite));
    CHECK(pw.edgePipeline == nullptr);
}
