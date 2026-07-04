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

TEST_CASE("TryDescribe round-trips factors, flags and texture handles") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);

    MaterialAsset a;
    a.baseColorFactor = {0.25f, 0.5f, 0.75f, 0.9f};
    a.metallicFactor = 0.6f;
    a.roughnessFactor = 0.15f;
    a.occlusionStrength = 0.8f;
    a.alphaCutoff = 0.33f;
    a.emissiveFactor = {1.0f, 0.4f, 0.1f};
    a.doubleSided = true;
    a.alphaMask = true;
    a.modulateVertexColor = true;
    a.albedoTex = treg.Acquire("brick.png");
    REQUIRE(a.albedoTex.IsValid());

    const MaterialHandle h = reg.Acquire(a);
    REQUIRE(h.IsValid());

    MaterialAsset out;
    REQUIRE(reg.TryDescribe(h, out));
    CHECK(out.baseColorFactor.x == doctest::Approx(0.25f));
    CHECK(out.baseColorFactor.w == doctest::Approx(0.9f));
    CHECK(out.metallicFactor == doctest::Approx(0.6f));
    CHECK(out.roughnessFactor == doctest::Approx(0.15f));
    CHECK(out.occlusionStrength == doctest::Approx(0.8f));
    CHECK(out.alphaCutoff == doctest::Approx(0.33f));
    CHECK(out.emissiveFactor.r == doctest::Approx(1.0f));
    CHECK(out.doubleSided);
    CHECK(out.alphaMask);
    CHECK(out.modulateVertexColor);
    CHECK(!out.alphaBlend);
    CHECK(out.albedoTex == a.albedoTex);
    CHECK(!out.normalTex.IsValid());

    reg.Release(h);
}

TEST_CASE("TryDescribe rejects invalid and stale handles") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);

    MaterialAsset out;
    CHECK(!reg.TryDescribe(MaterialHandle{}, out));

    MaterialAsset a;
    a.baseColorFactor = {1, 0, 0, 1};
    const MaterialHandle h = reg.Acquire(a);
    REQUIRE(h.IsValid());
    reg.Release(h); // refcount 1 -> 0: slot freed, generation bumped
    CHECK(!reg.TryDescribe(h, out));
}

TEST_CASE("Instance setters seed from the entity's CURRENT material, not defaults") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    PipelineCache cache; FakePipelineFactory pf; cache.Initialize({}, std::ref(pf));
    World world;
    (void) world.Create(); // burn entity id 0 (treated as null)
    Entity e = world.Create();

    // Entity starts with a textured, tinted material (a glTF-spawn stand-in).
    MaterialAsset authored;
    authored.baseColorFactor = {0.9f, 0.2f, 0.1f, 1.0f};
    authored.roughnessFactor = 0.35f;
    authored.albedoTex = treg.Acquire("fox_albedo.png");
    REQUIRE(authored.albedoTex.IsValid());
    MaterialSystem::AssignMaterial(world, e, reg, cache, authored);

    // First single-field edit must preserve everything else - the old
    // default-seed behavior silently wiped textures and tint here.
    MaterialSystem::SetMetallic(world, e, reg, cache, 0.7f);

    const MaterialAsset& inst = world.Get<MaterialInstanceComponent>(e).asset;
    CHECK(inst.metallicFactor == doctest::Approx(0.7f));
    CHECK(inst.baseColorFactor.r == doctest::Approx(0.9f));
    CHECK(inst.roughnessFactor == doctest::Approx(0.35f));
    CHECK(inst.albedoTex == authored.albedoTex);

    // And the reassigned registry material carries the texture too.
    MaterialAsset described;
    REQUIRE(reg.TryDescribe(world.Get<MaterialComponent>(e).handle, described));
    CHECK(described.albedoTex == authored.albedoTex);
    CHECK(described.metallicFactor == doctest::Approx(0.7f));
}
