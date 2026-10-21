#include <doctest/doctest.h>
#include <cstddef>
#include "gpu/SpritePass.hpp"
#include "gpu/AsyncRenderLifecycle.hpp"
#include "gpu/MeshPass.hpp"
#include <limits>
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("WebGPU sprite bounds ordering and asynchronous retirement")
{
	using namespace aether;
	static_assert(sizeof(gpu::SpriteGpuInstance) == 128);
	static_assert(offsetof(gpu::SpriteGpuInstance, color) == 80);
	Render2DFrameData frame;
	for (unsigned i = 0; i < 7; ++i)
	{
		SpriteRenderInstance sprite;
		sprite.entityId = i;
		sprite.textureIndex = i < 4 ? 0 : 1;
		sprite.sortKey = 6 - i;
		frame.sprites.push_back(sprite);
	}
	auto plan = gpu::PlanSpritePass(frame, 2, 16);
	REQUIRE(plan.has_value());
	CHECK(plan->instances.size() == 7);
	CHECK(plan->batches.size() == 4);
	CHECK(plan->instances.front().metadata.y == 6);
	CHECK(plan->batches[0].texture == 1);
	CHECK(plan->batches[0].count == 2);
	CHECK(!gpu::PlanSpritePass(frame, 2, 6));
	CHECK(!gpu::PlanSpritePass(frame, 0, 16));
	frame.sprites[0].blendMode = 99;
	CHECK(!gpu::PlanSpritePass(frame, 2, 16));
	gpu::AsyncRenderLifecycle life;
	CHECK(life.State() == gpu::RenderState::Initializing);
	life.Stop();
	CHECK(!life.Ready());
	CHECK(life.State() == gpu::RenderState::Stopped);
	gpu::AsyncRenderLifecycle live;
	CHECK(live.Ready());
	CHECK(live.Submit());
	CHECK(live.Submit());
	live.Stop();
	CHECK(!live.Submit());
	CHECK(live.State() == gpu::RenderState::Stopping);
	live.Complete();
	CHECK(live.State() == gpu::RenderState::Stopping);
	live.Complete();
	CHECK(live.State() == gpu::RenderState::Stopped);
	gpu::AsyncRenderLifecycle lost;
	CHECK(lost.Ready());
	CHECK(lost.Submit());
	lost.Fail(true);
	CHECK(!lost.Submit());
	lost.Fail();
	lost.Complete();
	CHECK(lost.State() == gpu::RenderState::DeviceLost);
}

TEST_CASE("Portable mesh frames validate transforms and preserve draw order")
{
	using namespace aether;
	Render3DFrameData frame;
	frame.meshes.push_back({.world = glm::scale(glm::mat4(1), glm::vec3(-2, 4, 0.5f)), .color = {1, 0, 0, 1}, .mesh = 7});
	frame.meshes.push_back({.color = {0, 0, 1, 1}, .mesh = 3});
	const auto planned = gpu::PlanMeshPass(frame);
	REQUIRE(planned);
	CHECK(planned->size() == 2);
	CHECK(planned->at(0).normal0.x == doctest::Approx(-0.5f));
	CHECK(planned->at(0).normal1.y == doctest::Approx(0.25f));
	CHECK(planned->at(0).normal2.z == doctest::Approx(2));
	CHECK(planned->at(1).color.b == 1);
	CHECK_FALSE(gpu::PlanMeshPass(frame, 1));
	frame.meshes[0].world[0][0] = 0;
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].world = glm::mat4(1);
	frame.meshes[0].world[3][3] = 0;
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].world = glm::mat4(1);
	frame.meshes[0].world[2][1] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].world = glm::mat4(1);
	frame.meshes[0].color.g = std::numeric_limits<float>::infinity();
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	CHECK(gpu::PlanMeshPass(Render3DFrameData{}));
}
