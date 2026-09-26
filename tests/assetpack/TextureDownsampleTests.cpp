// Regression tests for the alpha-weighted mip downsample in DDSFormat.hpp.
//
// PS2 cut-out textures (Twinsanity grass/leaves) store pure black RGB in their
// fully transparent texels. An unweighted 2x2 average bleeds that black into
// every mip level, which is what made the nitro-field grass go muddy/black with
// distance. The downsample must weight RGB by alpha instead.
#include <doctest/doctest.h>

#include "DDSFormat.hpp"

#include <vector>

using aether::assetpipeline::DilateTransparentRgb;
using aether::assetpipeline::DownsampleBox2x2;

namespace
{
	struct Quad
	{
		uint8_t px[16];
	};
} // namespace

TEST_CASE("Alpha-weighted downsample keeps cut-out texel colour out of the black fringe")
{
	// One opaque green texel, three fully transparent BLACK texels.
	Quad q = {{
		0, 255, 0, 255,   0, 0, 0, 0,
		0, 0, 0, 0,       0, 0, 0, 0,
	}};
	uint8_t dst[4] = {1, 1, 1, 1};

	DownsampleBox2x2(q.px, 2, 2, 4, dst);

	// The old unweighted average produced rgb=(0,64,0): the mip darkened by 3/4.
	CHECK(dst[0] == 0);
	CHECK(dst[1] == 255); // visible texel's colour preserved
	CHECK(dst[2] == 0);
	CHECK(dst[3] == 64);  // coverage still quarters (plain alpha average)
}

TEST_CASE("Alpha-weighted downsample averages two opaque texels, not one winning")
{
	Quad q = {{
		0, 255, 0, 255,   255, 0, 0, 255,
		0, 0, 0, 0,       0, 0, 0, 0,
	}};
	uint8_t dst[4];

	DownsampleBox2x2(q.px, 2, 2, 4, dst);

	CHECK(dst[0] == 128);
	CHECK(dst[1] == 128);
	CHECK(dst[2] == 0);
	CHECK(dst[3] == 128);
}

TEST_CASE("Alpha-weighted downsample leaves fully transparent quads transparent")
{
	Quad q = {}; // all zero
	uint8_t dst[4] = {9, 9, 9, 9};

	DownsampleBox2x2(q.px, 2, 2, 4, dst);

	CHECK(dst[0] == 0);
	CHECK(dst[1] == 0);
	CHECK(dst[2] == 0);
	CHECK(dst[3] == 0);
}

TEST_CASE("Alpha-weighted downsample keeps the opaque 3-channel average unweighted")
{
	// rgb rows (10,20,30) (20,40,60) / (30,60,90) (40,80,120) -> plain average (25,50,75).
	const uint8_t q[12] = {
		10, 20, 30,   20, 40, 60,
		30, 60, 90,   40, 80, 120,
	};
	uint8_t dst[3];

	DownsampleBox2x2(q, 2, 2, 3, dst);

	CHECK(dst[0] == 25);
	CHECK(dst[1] == 50);
	CHECK(dst[2] == 75);
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
