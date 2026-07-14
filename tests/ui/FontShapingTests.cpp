#include <doctest/doctest.h>

#include "ui/FontRegistry.hpp"

using namespace aether::ui;

static FontAsset MakeMonoFont() {
    FontAsset f;
    f.atlasWidth = f.atlasHeight = 128;
    f.ascent = 40;
    f.descent = 10;
    f.lineHeight = 50;
    f.bakeSize = 48;
    for (std::uint32_t c = 'A'; c <= 'Z'; ++c) {
        f.glyphs[c] = GlyphMeta{c, 0, 0, 0.1f, 0.1f, 20, 30, 0, 30, 24};
    }
    f.glyphs[' '] = GlyphMeta{' ', 0, 0, 0, 0, 0, 0, 0, 0, 24};
    return f;
}

TEST_CASE("ShapeText advances left-to-right at the glyph advance") {
    const auto f = MakeMonoFont();
    const auto g = ShapeText(f, "AB", 48.f, {0, 0, 1000, 100}, false, 0, 0);
    REQUIRE(g.size() == 2);
    CHECK(g[0].rect.x == doctest::Approx(0.f));
    CHECK(g[0].rect.y == doctest::Approx(10.f));
    CHECK(g[0].rect.z == doctest::Approx(20.f));
    CHECK(g[1].rect.x - g[0].rect.x == doctest::Approx(24.f));
}

TEST_CASE("ShapeText scales by pixelSize/bakeSize") {
    const auto f = MakeMonoFont();
    const auto g = ShapeText(f, "AB", 24.f, {0, 0, 1000, 100}, false, 0, 0);
    CHECK(g[1].rect.x - g[0].rect.x == doctest::Approx(12.f));
    CHECK(g[0].rect.z == doctest::Approx(10.f));
}

TEST_CASE("ShapeText wraps at the box width") {
    const auto f = MakeMonoFont();
    const auto g = ShapeText(f, "AAAAA", 48.f, {0, 0, 50, 200}, true, 0, 0);
    REQUIRE(g.size() == 5);
    CHECK(g.back().rect.y > g.front().rect.y);
}

TEST_CASE("ShapeText center-aligns a line") {
    const auto f = MakeMonoFont();
    const auto g = ShapeText(f, "AB", 48.f, {0, 0, 1000, 100}, false, 1, 0);
    CHECK(g[0].rect.x == doctest::Approx(476.f));
}

TEST_CASE("ShapeText middle-valigns a single line") {
    const auto f = MakeMonoFont();
    const auto g = ShapeText(f, "A", 48.f, {0, 0, 100, 100}, false, 0, 1);
    CHECK(g[0].rect.y == doctest::Approx(35.f));
}
