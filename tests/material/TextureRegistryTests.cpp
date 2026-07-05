#include <doctest/doctest.h>
#include "material/TextureRegistry.hpp"
#include "FakeTextureSink.hpp"

using namespace aether;

TEST_CASE("Acquire returns a valid handle and loads once") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle h = reg.Acquire("brick.png");
    CHECK(h.IsValid());
    CHECK(sink.loadCount == 1);
    CHECK(reg.ResolveSlot(h) != TextureResource::kInvalidSlot);
}

TEST_CASE("Same resolved path dedups; two spellings still dedup") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    TextureHandle b = reg.Acquire("brick.png");     // exact same string
    TextureHandle c = reg.Acquire("brick.texture"); // different spelling, same resolved path

    CHECK(a == b);
    CHECK(a == c);              // deduped because ResolvePath runs BEFORE hashing
    CHECK(sink.loadCount == 1); // loaded exactly once
    CHECK(reg.ResolveSlot(a) == reg.ResolveSlot(c));
}

TEST_CASE("TryGetPath returns the resolved path for live handles only") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle h = reg.Acquire("brick.png");
    REQUIRE(h.IsValid());

    std::string path;
    CHECK(reg.TryGetPath(h, path));
    CHECK(!path.empty());

    // Re-acquiring by the returned path must dedup onto the same entry - the
    // path IS the texture's stable identity for scene serialization.
    CHECK(reg.Acquire(path) == h);

    CHECK(!reg.TryGetPath(TextureHandle{}, path));
    CHECK(!reg.TryGetPath(TextureHandle::Broken(), path));
}

TEST_CASE("Distinct paths get distinct entries and slots") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    TextureHandle b = reg.Acquire("moss.png");

    CHECK(a.index != b.index);
    CHECK(reg.ResolveSlot(a) != reg.ResolveSlot(b));
    CHECK(sink.loadCount == 2);
}

TEST_CASE("Refcount: shared entry survives one release, frees at zero") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    TextureHandle b = reg.Acquire("brick.png"); // refcount 2, same entry
    reg.Release(a);
    CHECK(reg.ResolveSlot(b) != TextureResource::kInvalidSlot); // still resident
    reg.Release(b);
    CHECK(reg.ResolveSlot(b) == reg.ResolveSlot(reg.DefaultHandle())); // now stale -> default
}

TEST_CASE("Generation invalidates stale handles after free + reload") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    TextureHandle stale = reg.Acquire("brick.png");
    reg.Release(stale); // entry 0 dead, generation bumped

    TextureHandle fresh = reg.Acquire("moss.png"); // reuses entry 0, new generation
    CHECK(fresh.index == stale.index);
    CHECK(fresh.generation != stale.generation);
    CHECK(reg.ResolveSlot(stale) == reg.ResolveSlot(reg.DefaultHandle())); // stale -> default
    CHECK(reg.ResolveSlot(fresh) != reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("Default handle resolves; invalid handle falls back to default") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    reg.InitializeDefault("magenta.png");

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(TextureHandle{}) == reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("InitializeDefault(TextureResource) installs a synthesized fallback without touching the sink") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    reg.InitializeDefault(TextureResource{42u}); // code-synthesized, no path load

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) == 42u);
    CHECK(reg.ResolveSlot(TextureHandle{}) == 42u);   // invalid handle -> default
    CHECK(sink.loadCount == 0);                        // never hit the sink

    // The synthesized default is not a dedup target: a real Acquire still loads.
    TextureHandle a = reg.Acquire("brick.png");
    CHECK(a.IsValid());
    CHECK(sink.loadCount == 1);
    CHECK(reg.ResolveSlot(a) != 42u);

    // A stale handle resolves to the synthesized default, not its old slot.
    reg.Release(a);
    CHECK(reg.ResolveSlot(a) == 42u);
}

TEST_CASE("AddRef bumps an existing entry so it survives an extra release") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png"); // refcount 1
    reg.AddRef(a);                              // refcount 2 (no reload)
    CHECK(sink.loadCount == 1);
    reg.Release(a);                             // -> 1, still resident
    CHECK(reg.ResolveSlot(a) != reg.ResolveSlot(reg.DefaultHandle()));
    reg.Release(a);                             // -> 0, freed
    CHECK(reg.ResolveSlot(a) == reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("Failed Acquire returns a broken handle that resolves to the default; Release is a no-op") {
    FakeTextureSink sink(1);
    TextureRegistry reg(sink);
    reg.InitializeDefault("magenta.png"); // consumes the only slot

    TextureHandle h = reg.Acquire("brick.png"); // sink full -> load fails
    CHECK(h.IsValid());                          // broken is VALID (routes to magenta), unlike "no texture"
    CHECK(h == TextureHandle::Broken());
    CHECK(reg.ResolveSlot(h) == reg.ResolveSlot(reg.DefaultHandle())); // visible magenta fallback
    reg.Release(h); // must be a safe no-op (touches no live refcount)
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) != TextureResource::kInvalidSlot); // default intact
}

TEST_CASE("Broken handle is distinct from invalid: valid, resolves to default, ref-op-safe") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    reg.InitializeDefault(TextureResource{7u});

    const TextureHandle broken = TextureHandle::Broken();
    const TextureHandle absent{}; // "no texture requested"

    CHECK(broken.IsValid());     // -> packing routes through ResolveSlot -> magenta default
    CHECK_FALSE(absent.IsValid()); // -> packing emits kNoTexture (base colour)
    CHECK(reg.ResolveSlot(broken) == 7u);

    // Ref ops on a broken handle never touch a live entry, so they cannot corrupt
    // the default or any real texture's refcount.
    reg.AddRef(broken);
    reg.Release(broken);
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) == 7u);

    // A real texture acquired alongside is unaffected.
    TextureHandle a = reg.Acquire("brick.png");
    CHECK(a.IsValid());
    CHECK(reg.ResolveSlot(a) != 7u);
}
