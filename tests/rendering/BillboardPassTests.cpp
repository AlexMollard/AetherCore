#include <doctest/doctest.h>

#include "gpu/BillboardPass.hpp"

using namespace aether;

TEST_CASE("Billboard planning preserves stable depth order and blend flags")
{
	std::vector<BillboardParticleInstance> particles(4);
	for (unsigned i = 0; i < particles.size(); ++i)
	{
		particles[i].positionSize = glm::vec4(0, 0, i == 0 ? -2 : i == 2 ? -3 : -5, 1);
		particles[i].entityId = i + 1;
	}
	particles[1].blendMode = kBillboardAdditive;
	particles[3].blendMode = kBillboardDisplaySpace | kBillboardLit;
	const auto plan = gpu::PlanBillboardPass(particles, glm::mat4(1), 1);
	REQUIRE(plan);
	CHECK((*plan)[0].entityId == 2);
	CHECK((*plan)[1].entityId == 4);
	CHECK((*plan)[2].entityId == 3);
	CHECK((*plan)[3].entityId == 1);
	CHECK((*plan)[0].blendMode == kBillboardAdditive);
	CHECK((*plan)[1].blendMode == (kBillboardDisplaySpace | kBillboardLit));
}

TEST_CASE("Billboard draw budget counts sorted contiguous runs in both render phases")
{
	std::vector<BillboardParticleInstance> particles(4);
	for (unsigned i = 0; i < particles.size(); ++i)
	{
		particles[i].positionSize = glm::vec4(0, 0, -float(10 - i), 1);
		particles[i].textureIndex = i % 2;
	}
	CHECK_FALSE(gpu::PlanBillboardPass(particles, glm::mat4(1), 2, 100, 3));
	CHECK(gpu::PlanBillboardPass(particles, glm::mat4(1), 2, 100, 4));
	for (auto& particle: particles)
	{
		particle.textureIndex = 0;
	}
	CHECK(gpu::PlanBillboardPass(particles, glm::mat4(1), 1, 100, 1));
	particles[1].blendMode = kBillboardAdditive;
	CHECK_FALSE(gpu::PlanBillboardPass(particles, glm::mat4(1), 1, 100, 2));
	particles[1].blendMode = kBillboardDisplaySpace;
	CHECK(gpu::PlanBillboardPass(particles, glm::mat4(1), 1, 100, 2));
}

TEST_CASE("Billboard planning rejects malformed data before GPU allocations")
{
	BillboardParticleInstance particle;
	particle.positionSize = glm::vec4(0, 0, -2, 1);
	auto plan = [&]
	{
		return gpu::PlanBillboardPass(std::span(&particle, 1), glm::mat4(1), 1);
	};
	REQUIRE(plan());
	particle.textureIndex = 1;
	CHECK_FALSE(plan());
	particle.textureIndex = UINT32_MAX;
	CHECK(plan());
	particle.positionSize.w = -1;
	CHECK_FALSE(plan());
	particle.positionSize.w = 1;
	particle.color.x = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(plan());
	particle.color.x = 1;
	particle.blendMode = 1u << 7;
	CHECK_FALSE(plan());
	particle.blendMode = 0;
	CHECK_FALSE(gpu::PlanBillboardPass(std::span(&particle, 1), glm::mat4(0), 1));
	CHECK_FALSE(gpu::PlanBillboardPass(std::span(&particle, 1), glm::mat4(1), 1, 0));
}
