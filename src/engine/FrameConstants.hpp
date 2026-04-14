#pragma once

#include <glm/glm.hpp>

namespace aether
{
	// Per-frame constant data written once to a GPU uniform buffer before any draws.
	// Consumed by all shaders via descriptor set 0, binding 0.
	struct FrameConstants
	{
		glm::mat4 viewProj{ 1.0f };
		glm::mat4 view{ 1.0f };
		glm::mat4 proj{ 1.0f };
	};
}
