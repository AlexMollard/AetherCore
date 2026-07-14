#pragma once

namespace aether
{
	class RenderQueue;
	class World;

	namespace WorldRenderer
	{
		void Flush(const World& world, RenderQueue& queue, bool shadowPass = false);
	}
} // namespace aether
