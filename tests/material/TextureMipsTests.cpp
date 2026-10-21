#include <doctest/doctest.h>
#include <TextureMips.hpp>

#include <array>

using namespace aether;

TEST_CASE("RGBA mip planning accounts for the complete chain before allocation")
{
	const auto full = PlanRgbaTextureMips(4, 4, true, 84);
	REQUIRE(full);
	CHECK(full->levelCount == 3);
	CHECK(full->totalBytes == 84);
	CHECK_FALSE(PlanRgbaTextureMips(4, 4, true, 83));
	CHECK_FALSE(PlanRgbaTextureMips(4, 4, true, 64));
	const auto base = PlanRgbaTextureMips(4, 4, false, 64);
	REQUIRE(base);
	CHECK(base->levelCount == 1);
	CHECK(base->totalBytes == 64);
	CHECK_FALSE(PlanRgbaTextureMips(0, 4, true, 1024));
	CHECK_FALSE(PlanRgbaTextureMips(4, 0, true, 1024));
	CHECK_FALSE(PlanRgbaTextureMips(UINT32_MAX, UINT32_MAX, true, SIZE_MAX));
	CHECK_FALSE(PlanRgbaTextureMips(1, 1, true, 3));
	CHECK_FALSE(PlanRgbaTextureMips(32768, 32768, true, SIZE_MAX));
	CHECK_FALSE(PlanRgbaTextureMips(1, 536870912, true, SIZE_MAX));
}

TEST_CASE("RGBA mip chains preserve native floor dimensions and single axis filtering")
{
	for (const auto [width, height]: {std::pair{7u, 3u}, std::pair{1u, 7u}, std::pair{7u, 1u}})
	{
		std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4, 255);
		const auto levels = BuildRgbaTextureMips(pixels, width, height, true, 1024);
		REQUIRE(levels);
		REQUIRE(levels->size() == 3);
		CHECK((*levels)[1].width == std::max(1u, width / 2));
		CHECK((*levels)[1].height == std::max(1u, height / 2));
		CHECK(levels->back().width == 1);
		CHECK(levels->back().height == 1);
		CHECK(levels->back().pixels == std::vector<std::uint8_t>(4, 255));
	}
	// The native cooker drops unmatched trailing rows/columns on odd dimensions.
	std::array<std::uint8_t, 12> odd{10, 20, 30, 255, 30, 40, 50, 255, 250, 250, 250, 255};
	const auto levels = BuildRgbaTextureMips(odd, 3, 1, true, 16);
	REQUIRE(levels);
	CHECK(levels->back().pixels == std::vector<std::uint8_t>{20, 30, 40, 255});
}

TEST_CASE("RGBA mip colors match published encoded byte filtering")
{
	const std::array<std::uint8_t, 8> opaque{0, 0, 0, 255, 255, 255, 255, 255};
	const auto levels = BuildRgbaTextureMips(opaque, 2, 1, true, 12);
	REQUIRE(levels);
	CHECK(levels->back().pixels == std::vector<std::uint8_t>{128, 128, 128, 255});
	const std::array<std::uint8_t, 8> cutout{200, 100, 50, 255, 0, 0, 0, 0};
	const auto alpha = BuildRgbaTextureMips(cutout, 2, 1, true, 12);
	REQUIRE(alpha);
	CHECK((*alpha)[0].pixels == std::vector<std::uint8_t>{200, 100, 50, 255, 200, 100, 50, 0});
	CHECK(alpha->back().pixels == std::vector<std::uint8_t>{200, 100, 50, 128});
	// Existing native alpha is sum(alpha squared)/(count*255), not a plain average.
	const std::array<std::uint8_t, 8> translucent{200, 100, 50, 128, 100, 200, 150, 128};
	const auto partial = BuildRgbaTextureMips(translucent, 2, 1, true, 12);
	REQUIRE(partial);
	CHECK(partial->back().pixels == std::vector<std::uint8_t>{150, 150, 100, 64});
	const std::array<std::uint8_t, 8> transparent{};
	const auto empty = BuildRgbaTextureMips(transparent, 2, 1, true, 12);
	REQUIRE(empty);
	CHECK(empty->back().pixels == std::vector<std::uint8_t>(4, 0));
}

TEST_CASE("RGBA base only uploads preserve source bytes and validate payloads")
{
	const std::array<std::uint8_t, 8> source{200, 100, 50, 255, 1, 2, 3, 0};
	const auto base = BuildRgbaTextureMips(source, 2, 1, false, 8);
	REQUIRE(base);
	REQUIRE(base->size() == 1);
	CHECK(base->front().pixels == std::vector<std::uint8_t>(source.begin(), source.end()));
	CHECK_FALSE(BuildRgbaTextureMips(source, 2, 1, true, 11));
	CHECK_FALSE(BuildRgbaTextureMips(source, 3, 1, true, 1024));
	const auto single = BuildRgbaTextureMips(std::span(source).first(4), 1, 1, true, 4);
	REQUIRE(single);
	CHECK(single->size() == 1);
}
