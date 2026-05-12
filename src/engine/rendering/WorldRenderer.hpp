#pragma once

namespace aether
{
	class IAnimationProvider;
	class RenderQueue;
	class Scene;
	class World;

	// Free functions that flush ECS and legacy scene data into a render queue.
	// Decouples World/Scene from the RenderQueue header dependency.
	namespace WorldRenderer
	{
		void Flush(const World& world, RenderQueue& queue, const IAnimationProvider* anim = nullptr);
		void Flush(const Scene& scene, RenderQueue& queue);
	} // namespace WorldRenderer
} // namespace aether
