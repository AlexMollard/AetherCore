#pragma once

namespace aether
{
	class IAnimationProvider;
	class RenderQueue;
	class World;

	namespace WorldRenderer
	{
		void Flush(const World& world, RenderQueue& queue, const IAnimationProvider* anim = nullptr);
	} // namespace WorldRenderer
} // namespace aether
