#include <doctest/doctest.h>

#include "DDSFormat.hpp"

#include <vector>

using aether::assetpipeline::DilateTransparentRgb;
using aether::assetpipeline::DownsampleBox2x2;


TEST_CASE("Published texture reduction uses linear light for color and byte averages for data")
{
	const uint8_t rgba[] = {0, 0, 0, 128, 255, 255, 255, 128};
	uint8_t dst[4]{};
	DownsampleBox2x2(rgba, 2, 1, 4, dst, true);
	CHECK(dst[0] == 188);
	CHECK(dst[1] == 188);
	CHECK(dst[2] == 188);
	CHECK(dst[3] == 128);
	DownsampleBox2x2(rgba, 2, 1, 4, dst, false);
	CHECK(dst[0] == 128);
	CHECK(dst[1] == 128);
	CHECK(dst[2] == 128);
	CHECK(dst[3] == 128);
	const uint8_t rgb[] = {10, 20, 30, 20, 40, 60, 30, 60, 90, 40, 80, 120};
	DownsampleBox2x2(rgb, 2, 2, 3, dst, false);
	CHECK(dst[0] == 25);
	CHECK(dst[1] == 50);
	CHECK(dst[2] == 75);
	const uint8_t coverage[] = {255, 0, 0, 255, 0, 0, 255, 0};
	DownsampleBox2x2(coverage, 2, 1, 4, dst, true);
	CHECK(dst[0] == 255);
	CHECK(dst[1] == 0);
	CHECK(dst[2] == 0);
	CHECK(dst[3] == 128);
}

TEST_CASE("Linear data mip channels ignore alpha coverage and preserve transparent source data")
{
	const uint8_t data[] = {0, 0, 255, 64, 255, 0, 0, 255};
	uint8_t reduced[4]{};
	DownsampleBox2x2(data, 2, 1, 4, reduced, false);
	CHECK(reduced[0] == 128);
	CHECK(reduced[2] == 128);
	CHECK(reduced[3] == 160);

	const uint8_t transparent[] = {0, 0, 255, 0, 255, 0, 0, 255};
	const auto levels = aether::BuildRgbaTextureMips(transparent, 2, 1, true, 12, false);
	REQUIRE(levels.has_value());
	CHECK(levels->front().pixels[2] == 255);
	CHECK(levels->back().pixels[0] == 128);
	CHECK(levels->back().pixels[2] == 128);
	CHECK(levels->back().pixels[3] == 128);
}

TEST_CASE("Transparent texels take their visible neighbours' colour, so a cut-out edge filters without a dark outline")
{
	// 4x1: green, red opaque at the ends; two black fully transparent texels between them.
	uint8_t px[16] = {
		0, 200, 0, 255,   0, 0, 0, 0,   0, 0, 0, 0,   200, 0, 0, 255,
	};
	DilateTransparentRgb(px, 4, 1, 4);
	// Each gap texel takes its one visible neighbour, and stays invisible.
	CHECK(px[4] == 0);
	CHECK(px[5] == 200);
	CHECK(px[7] == 0);
	CHECK(px[8] == 200);
	CHECK(px[9] == 0);
	CHECK(px[11] == 0);
	// Visible texels never change.
	CHECK(px[1] == 200);
	CHECK(px[12] == 200);
}

TEST_CASE("Transparent texels beyond the flood reach take the mean visible colour, never black")
{
	std::vector<uint8_t> px(64 * 4, 0);
	px[0] = 100; px[1] = 150; px[2] = 50; px[3] = 255; // one visible texel at the left end of 64x1
	DilateTransparentRgb(px.data(), 64, 1, 4);
	CHECK(px[63 * 4 + 0] == 100);
	CHECK(px[63 * 4 + 1] == 150);
	CHECK(px[63 * 4 + 3] == 0);
}
