#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "FakeSlotSink.hpp"
#include "FakeTextureSink.hpp"

using namespace aether;

TEST_CASE("Acquire returns a valid handle and writes the slot") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);

    MaterialAsset a; a.baseColorFactor = {1,0,0,1};
    MaterialHandle h = reg.Acquire(a);

    CHECK(h.IsValid());
    CHECK(sink.writeCount == 1);
    CHECK(reg.ResolveSlot(h) == h.index);
}

TEST_CASE("Identical assets dedup to one slot; distinct assets do not") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);

    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialAsset red2; red2.baseColorFactor = {1,0,0,1};
    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};

    MaterialHandle h1 = reg.Acquire(red);
    MaterialHandle h2 = reg.Acquire(red2);
    MaterialHandle h3 = reg.Acquire(blue);

    CHECK(h1 == h2);
    CHECK(h1.index != h3.index);
    CHECK(sink.allocCount == 2);
}

TEST_CASE("Refcount: shared slot survives one release, frees at zero") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    MaterialAsset a; a.baseColorFactor = {1,0,0,1};

    MaterialHandle h1 = reg.Acquire(a);
    MaterialHandle h2 = reg.Acquire(a);
    reg.Release(h1);
    CHECK(sink.freeCount == 0);
    CHECK(reg.ResolveSlot(h2) == h2.index);
    reg.Release(h2);
    CHECK(sink.freeCount == 1);
}

TEST_CASE("Generation invalidates stale handles after free+realloc") {
    FakeSlotSink sink(1);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);

    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialHandle stale = reg.Acquire(red);
    reg.Release(stale);

    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};
    MaterialHandle fresh = reg.Acquire(blue);

    CHECK(fresh.IsValid());
    CHECK(fresh.index == stale.index);
    CHECK(fresh.generation != stale.generation);
    CHECK(reg.ResolveSlot(stale) == reg.ResolveSlot(reg.DefaultHandle()));
    CHECK(reg.ResolveSlot(fresh) == fresh.index);
}

TEST_CASE("Default handle resolves; invalid handle falls back to default") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    MaterialAsset def; def.baseColorFactor = {0.85f,0.85f,0.82f,1.0f};
    reg.InitializeDefault(def);

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(MaterialHandle{}) == reg.DefaultHandle().index);
}

TEST_CASE("Acquiring a material bumps its textures' refcounts; release drops them") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    FakeSlotSink matSink(8);
    MaterialRegistry mat(matSink, tex);

    MaterialAsset a;
    a.albedoTex = tex.Acquire("brick.png");
    MaterialHandle m = mat.Acquire(a);

    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle()));
    mat.Release(m);
    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle()));
    tex.Release(a.albedoTex);
    CHECK(tex.ResolveSlot(a.albedoTex) == tex.ResolveSlot(tex.DefaultHandle()));
}

TEST_CASE("Two entities sharing one textured material hold one texture ref (per-slot)") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    FakeSlotSink matSink(8);
    MaterialRegistry mat(matSink, tex);

    MaterialAsset a;
    a.albedoTex = tex.Acquire("brick.png");
    MaterialHandle m1 = mat.Acquire(a);
    MaterialHandle m2 = mat.Acquire(a);

    mat.Release(m1);
    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle()));
    mat.Release(m2);
    tex.Release(a.albedoTex);
    CHECK(tex.ResolveSlot(a.albedoTex) == tex.ResolveSlot(tex.DefaultHandle()));
}

TEST_CASE("Buffer-full Acquire does not corrupt the default material") {
    FakeSlotSink sink(1);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    MaterialAsset def; def.baseColorFactor = {0.5f, 0.5f, 0.5f, 1};
    reg.InitializeDefault(def);

    MaterialAsset other; other.baseColorFactor = {1, 0, 0, 1};
    MaterialHandle h = reg.Acquire(other);

    CHECK_FALSE(h.IsValid());
    CHECK(reg.ResolveSlot(h) == reg.DefaultHandle().index);
    reg.Release(h);                                            // must be a safe no-op
    // Default must still be intact after releasing the fallback handle:
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) == reg.DefaultHandle().index);
}
