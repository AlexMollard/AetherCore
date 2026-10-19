#include <cassert>
#include <cstddef>
#include "gpu/SpritePass.hpp"
#include "gpu/AsyncRenderLifecycle.hpp"

int main()
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
	assert(plan && plan->instances.size() == 7 && plan->batches.size() == 4);
	assert(plan->instances.front().metadata.y == 6);
	assert(plan->batches[0].texture == 1 && plan->batches[0].count == 2);
	assert(!gpu::PlanSpritePass(frame, 2, 6));
	assert(!gpu::PlanSpritePass(frame, 0, 16));
	frame.sprites[0].blendMode = 99;
	assert(!gpu::PlanSpritePass(frame, 2, 16));
	gpu::AsyncRenderLifecycle life;
	assert(life.State() == gpu::RenderState::Initializing);
	life.Stop();
	assert(!life.Ready());
	assert(life.State() == gpu::RenderState::Stopped);
	gpu::AsyncRenderLifecycle live;
	assert(live.Ready());
	assert(live.Submit());
	assert(live.Submit());
	live.Stop();
	assert(!live.Submit());
	assert(live.State() == gpu::RenderState::Stopping);
	live.Complete();
	assert(live.State() == gpu::RenderState::Stopping);
	live.Complete();
	assert(live.State() == gpu::RenderState::Stopped);
	gpu::AsyncRenderLifecycle lost;
	assert(lost.Ready());
	assert(lost.Submit());
	lost.Fail(true);
	assert(!lost.Submit());
	lost.Complete();
	assert(lost.State() == gpu::RenderState::DeviceLost);
}
