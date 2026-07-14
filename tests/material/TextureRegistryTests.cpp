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
    TextureHandle b = reg.Acquire("brick.png");
    TextureHandle c = reg.Acquire("brick.texture");

    CHECK(a == b);
    CHECK(a == c);
    CHECK(sink.loadCount == 1);
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
    TextureHandle b = reg.Acquire("brick.png");
    reg.Release(a);
    CHECK(reg.ResolveSlot(b) != TextureResource::kInvalidSlot);
    reg.Release(b);
    CHECK(reg.ResolveSlot(b) == reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("Generation invalidates stale handles after free + reload") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    TextureHandle stale = reg.Acquire("brick.png");
    reg.Release(stale);

    TextureHandle fresh = reg.Acquire("moss.png");
    CHECK(fresh.index == stale.index);
    CHECK(fresh.generation != stale.generation);
    CHECK(reg.ResolveSlot(stale) == reg.ResolveSlot(reg.DefaultHandle()));
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
    reg.InitializeDefault(TextureResource{42u});

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) == 42u);
    CHECK(reg.ResolveSlot(TextureHandle{}) == 42u);
    CHECK(sink.loadCount == 0);                        // never hit the sink

    TextureHandle a = reg.Acquire("brick.png");
    CHECK(a.IsValid());
    CHECK(sink.loadCount == 1);
    CHECK(reg.ResolveSlot(a) != 42u);

    reg.Release(a);
    CHECK(reg.ResolveSlot(a) == 42u);
}

TEST_CASE("AddRef bumps an existing entry so it survives an extra release") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    reg.AddRef(a);
    CHECK(sink.loadCount == 1);
    reg.Release(a);
    CHECK(reg.ResolveSlot(a) != reg.ResolveSlot(reg.DefaultHandle()));
    reg.Release(a);
    CHECK(reg.ResolveSlot(a) == reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("Failed Acquire returns a broken handle that resolves to the default; Release is a no-op") {
    FakeTextureSink sink(1);
    TextureRegistry reg(sink);
    reg.InitializeDefault("magenta.png");

    TextureHandle h = reg.Acquire("brick.png");
    CHECK(h.IsValid());
    CHECK(h == TextureHandle::Broken());
    CHECK(reg.ResolveSlot(h) == reg.ResolveSlot(reg.DefaultHandle()));
    reg.Release(h); // must be a safe no-op (touches no live refcount)
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) != TextureResource::kInvalidSlot);
}

TEST_CASE("Broken handle is distinct from invalid: valid, resolves to default, ref-op-safe") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    reg.InitializeDefault(TextureResource{7u});

    const TextureHandle broken = TextureHandle::Broken();
    const TextureHandle absent{};

    CHECK(broken.IsValid());     // -> packing routes through ResolveSlot -> magenta default
    CHECK_FALSE(absent.IsValid()); // -> packing emits kNoTexture (base colour)
    CHECK(reg.ResolveSlot(broken) == 7u);

    // Ref ops on a broken handle never touch a live entry, so they cannot corrupt
    reg.AddRef(broken);
    reg.Release(broken);
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) == 7u);

    TextureHandle a = reg.Acquire("brick.png");
    CHECK(a.IsValid());
    CHECK(reg.ResolveSlot(a) != 7u);
}
