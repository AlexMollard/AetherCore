#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "FakeSlotSink.hpp"

using namespace aether;

TEST_CASE("Acquire returns a valid handle and writes the slot") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);

    MaterialAsset a; a.baseColorFactor = {1,0,0,1};
    MaterialHandle h = reg.Acquire(a);

    CHECK(h.IsValid());
    CHECK(sink.writeCount == 1);
    CHECK(reg.ResolveSlot(h) == h.index);
}

TEST_CASE("Identical assets dedup to one slot; distinct assets do not") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);

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
    MaterialRegistry reg(sink);
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
    MaterialRegistry reg(sink);

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
    MaterialRegistry reg(sink);
    MaterialAsset def; def.baseColorFactor = {0.85f,0.85f,0.82f,1.0f};
    reg.InitializeDefault(def);

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(MaterialHandle{}) == reg.DefaultHandle().index);
}

TEST_CASE("Buffer-full Acquire does not corrupt the default material") {
    FakeSlotSink sink(1); // capacity 1 -> only the default fits
    MaterialRegistry reg(sink);
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
