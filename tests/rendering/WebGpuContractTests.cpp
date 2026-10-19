#include <doctest/doctest.h>
#include <cstddef>
#include "gpu/SpritePass.hpp"
#include "gpu/AsyncRenderLifecycle.hpp"

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
