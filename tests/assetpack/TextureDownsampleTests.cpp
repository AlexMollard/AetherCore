// Regression tests for the alpha-weighted mip downsample in DDSFormat.hpp.
//
// PS2 cut-out textures (Twinsanity grass/leaves) store pure black RGB in their
// fully transparent texels. An unweighted 2x2 average bleeds that black into
// every mip level, which is what made the nitro-field grass go muddy/black with
// distance. The downsample must weight RGB by alpha instead.
#include <doctest/doctest.h>

#include "DDSFormat.hpp"

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
