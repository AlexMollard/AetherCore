#pragma once

#include <glm/vec3.hpp>

namespace aether
{
	class RenderQueue;
	class World;

	namespace WorldRenderer
	{
		// eyeWorldPos is the viewpoint this flush is building draws FOR, and it exists so
		// transparent geometry can be ordered back to front. The queue deliberately does not
		// know about cameras - it sorts on a key it is handed - so the caller, which is the
		// only thing that knows which view it is rendering, supplies it. A shadow pass may
		// pass anything: depth-only rendering does not blend, so the order cannot matter.
		void Flush(const World& world, RenderQueue& queue, glm::vec3 eyeWorldPos, bool shadowPass = false);
	}
} // namespace aether
