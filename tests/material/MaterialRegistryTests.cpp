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

    CHECK(h1 == h2);            // deduped
    CHECK(h1.index != h3.index); // distinct
    CHECK(sink.allocCount == 2); // only 2 slots used for 3 acquires
}

TEST_CASE("Refcount: shared slot survives one release, frees at zero") {
    FakeSlotSink sink(8);
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    MaterialAsset a; a.baseColorFactor = {1,0,0,1};

    MaterialHandle h1 = reg.Acquire(a);
    MaterialHandle h2 = reg.Acquire(a); // refcount 2, same slot
    reg.Release(h1);
    CHECK(sink.freeCount == 0);         // still referenced
    CHECK(reg.ResolveSlot(h2) == h2.index);
    reg.Release(h2);
    CHECK(sink.freeCount == 1);         // now freed
}

TEST_CASE("Generation invalidates stale handles after free+realloc") {
    FakeSlotSink sink(1); // single slot forces reuse
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);

    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialHandle stale = reg.Acquire(red);
    reg.Release(stale); // slot 0 freed, generation bumped

    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};
    MaterialHandle fresh = reg.Acquire(blue); // reuses slot 0, new generation

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
    a.albedoTex = tex.Acquire("brick.png"); // refcount 1 (loader ref)
    MaterialHandle m = mat.Acquire(a);       // material-slot ref bumps albedoTex -> 2

    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle()));
    mat.Release(m);                          // drops albedoTex -> 1
    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle())); // loader ref keeps it
    tex.Release(a.albedoTex);                // -> 0, freed
    CHECK(tex.ResolveSlot(a.albedoTex) == tex.ResolveSlot(tex.DefaultHandle()));
}

TEST_CASE("Two entities sharing one textured material hold one texture ref (per-slot)") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    FakeSlotSink matSink(8);
    MaterialRegistry mat(matSink, tex);

    MaterialAsset a;
    a.albedoTex = tex.Acquire("brick.png"); // loader ref (1)
    MaterialHandle m1 = mat.Acquire(a);      // fresh slot: +1 texture ref (2)
    MaterialHandle m2 = mat.Acquire(a);      // dedup hit: NO extra texture ref (still 2)

    mat.Release(m1);                         // slot refcount 2->1, no texture drop
    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle()));
    mat.Release(m2);                         // slot frees: drops the one texture ref (1)
    tex.Release(a.albedoTex);                // loader ref -> 0, freed
    CHECK(tex.ResolveSlot(a.albedoTex) == tex.ResolveSlot(tex.DefaultHandle()));
}

TEST_CASE("Buffer-full Acquire does not corrupt the default material") {
    FakeSlotSink sink(1); // capacity 1 -> only the default fits
    FakeTextureSink tsink;
    TextureRegistry treg(tsink);
    MaterialRegistry reg(sink, treg);
    MaterialAsset def; def.baseColorFactor = {0.5f, 0.5f, 0.5f, 1};
    reg.InitializeDefault(def); // consumes the only slot

    MaterialAsset other; other.baseColorFactor = {1, 0, 0, 1};
    MaterialHandle h = reg.Acquire(other); // sink full -> fallback

    CHECK_FALSE(h.IsValid());                                  // invalid handle on buffer-full
    CHECK(reg.ResolveSlot(h) == reg.DefaultHandle().index);    // resolves to the default slot
    reg.Release(h);                                            // must be a safe no-op
    // Default must still be intact after releasing the fallback handle:
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) == reg.DefaultHandle().index);
}
