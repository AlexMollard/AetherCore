#include <doctest/doctest.h>

#include <cstdint>

#include "ui/FontRegistry.hpp"

using namespace aether::ui;

// The atlas-era fixture described this mono font in pixels at a bake size of 48.
// FontAsset carries no bake size any more - metrics are em units and the requested
// pixel size is the whole scale - so the same font is those pixel numbers divided
// through by that em. Every expected value below is unchanged as a result.
static constexpr float kFixtureEm = 48.f;

static GlyphCurve MonoGlyph(std::uint32_t cp, bool visible)
{
	GlyphCurve g{};
	g.codepoint = cp;
	g.minX = 0.f;
	g.minY = 0.f;
	g.maxX = visible ? 20.f / kFixtureEm : 0.f;
	g.maxY = visible ? 30.f / kFixtureEm : 0.f;
	g.advance = 24.f / kFixtureEm;
	g.bearingX = 0.f;
	g.bearingY = visible ? 30.f / kFixtureEm : 0.f;
	g.bandTexel = 0u;
	// No bands means no outline: ShapeText advances the pen and emits nothing, which
	// is how a blank is expressed now that there is no zero-sized atlas rect.
	g.bandCountX = visible ? std::uint16_t{1} : std::uint16_t{0};
	g.bandCountY = visible ? std::uint16_t{1} : std::uint16_t{0};
	return g;
}

static FontAsset MakeMonoFont() {
    FontAsset f;
    f.ascent = 40.f / kFixtureEm;
    f.descent = 10.f / kFixtureEm;
    f.lineHeight = 50.f / kFixtureEm;
    for (std::uint32_t c = 'A'; c <= 'Z'; ++c) {
        f.glyphs[c] = MonoGlyph(c, true);
    }
    f.glyphs[' '] = MonoGlyph(' ', false);
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

TEST_CASE("ShapeText scales by the requested pixel size") {
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

TEST_CASE("ShapeText trailing newline does not add a phantom line") {
    const auto f = MakeMonoFont();
    const auto plain = ShapeText(f, "A", 48.f, {0, 0, 100, 100}, false, 0, 1);
    const auto trailing = ShapeText(f, "A\n", 48.f, {0, 0, 100, 100}, false, 0, 1);
    REQUIRE(plain.size() == 1);
    REQUIRE(trailing.size() == 1);
    // A phantom empty line would inflate the middle-aligned block height by a full
    // lineHeight (50) and push the visible glyph down by half of that.
    CHECK(trailing[0].rect.y == doctest::Approx(plain[0].rect.y));
    CHECK(trailing[0].rect.y == doctest::Approx(35.f));
}
