#pragma once

#include <glm/glm.hpp>
#include "physics/PhysicsComponents.hpp"

namespace aether
{
	// DebugVertex: per-vertex line endpoint. Matches the layout in debug_vert.slang.
	struct DebugVertex
	{
		glm::vec3 position;
		glm::vec4 color;
	};

	// thread into the RenderFramePacket. The render thread replays these (picking
	struct PhysicsDebugInstance
	{
		glm::mat4 model{1.0f};
		glm::vec4 tint{1.0f};
		PhysicsShapeType shape = PhysicsShapeType::Box;
	};

} // namespace aether
