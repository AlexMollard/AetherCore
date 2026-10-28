#pragma once

#include <array>
#include <glm/glm.hpp>

namespace aether
{
	struct GrassBenderComponent
	{
		float radius = 0.85f;
		float strength = 0.8f;
	};

	struct GrassBenderFrame
	{
		std::array<glm::vec4, 8> positions{};
		std::array<glm::vec4, 8> parameters{};
	};
}
