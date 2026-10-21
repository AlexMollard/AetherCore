#include "gpu/HdrPass.hpp"
#include <doctest/doctest.h>
#include <limits>

TEST_CASE("HDR plan rejects allocation abuse and malformed display backgrounds")
{
	aether::gpu::HdrUniforms constants{};
	constants.settings = {0, 1, 0, 1};
	CHECK(aether::gpu::PlanHdrPass(constants, 640, 480));
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 0, 480));
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 4097, 480));
	constants.settings[0] = 11;
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 640, 480));
	constants.settings[0] = 0;
	constants.settings[1] = std::numeric_limits<float>::infinity();
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 640, 480));
	constants.settings[1] = 1;
	constants.settings[2] = 1;
	constants.settings[3] = 9;
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 640, 480));
	constants.settings[3] = 2;
	constants.stops[0][3] = .8f;
	constants.stops[1][3] = .2f;
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 640, 480));
}

TEST_CASE("HDR sky rejects degenerate camera and sun snapshots")
{
	aether::gpu::HdrUniforms constants{};
	constants.settings = {1, 1, 2, 0};
	constants.projection = {1, -1, 0, 0};
	constants.sunDirectionIntensity = {0, 1, 0, 1};
	for (unsigned index = 0; index < 4; ++index)
	{
		constants.view[index][index] = 1;
	}
	CHECK(aether::gpu::PlanHdrPass(constants, 640, 480));
	constants.view[1] = constants.view[0];
	CHECK_FALSE(aether::gpu::PlanHdrPass(constants, 640, 480));
}
