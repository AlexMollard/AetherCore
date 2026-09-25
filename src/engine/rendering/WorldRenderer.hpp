#pragma once

#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace aether
{
	class RenderQueue;
	class World;
	class MaterialRegistry;

	namespace WorldRenderer
	{
		// eyeWorldPos is the viewpoint this flush is building draws FOR, and it exists so
		// transparent geometry can be ordered back to front. The queue deliberately does not
		// know about cameras - it sorts on a key it is handed - so the caller, which is the
		// only thing that knows which view it is rendering, supplies it. A shadow pass may
		// pass anything: depth-only rendering does not blend, so the order cannot matter.
		void Flush(const World& world, RenderQueue& queue, glm::vec3 eyeWorldPos, bool shadowPass = false);

		// Contact blob shadows: one per object-lit actor (its visible object-lit primitives
		// merged under their shared parent), nearest the eye first, capped at kMaxBlobShadows.
		// Each is xyz = (centre x, bounds bottom y, centre z), w = blob radius.
		void GatherBlobShadows(const World& world, const MaterialRegistry& materials, glm::vec3 eyeWorldPos, std::vector<glm::vec4>& out);
	}
} // namespace aether
