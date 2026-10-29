#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "ui/UiDrawCommand.hpp"

namespace aether::ui
{
	struct UiEffectDraw
	{
		std::string shader;
		glm::vec4 rect{0.f};
		glm::vec4 params{0.f};
		glm::vec4 color0{0.f};
		glm::vec4 color1{0.f};
		bool background = false;
		int sortOrder = 0;
	};

	struct UiFrameData
	{
		std::vector<UiDrawCommand> commands;
		std::vector<UiMaterialDraw> materials;
		std::vector<UiEffectDraw> effects;
	};
}
