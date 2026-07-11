#pragma once

namespace aether
{
	class RenderQueue;
	class World;

	namespace WorldRenderer
	{
		// Populates `queue` from the drawable entities in `world`. The same function
		// fills the main/RTT color queues and the shadow queues, each re-iterating
		// the ECS. `shadowPass` marks the shadow-map queues so a MeshRenderer with
		// castShadows==false is submitted to color passes but skipped for shadows.
		void Flush(const World& world, RenderQueue& queue, bool shadowPass = false);
	} // namespace WorldRenderer
} // namespace aether
