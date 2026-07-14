#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace aether::ui
{
	enum : std::uint32_t
	{
		kShapeRect = 0,
		kShapeCircle = 1,
		kShapeLine = 2,
		kShapeTexturedRect = 3,
		kShapeSdfGlyph = 4,
	};

	struct UiDrawCommand
	{
		glm::vec4 data0{0.f};
		glm::vec4 data1{0.f};
		glm::vec4 color{1.f};
		std::uint32_t type = kShapeRect;
		std::int32_t layer = 0;
		std::uint32_t textureSlot = 0;
		std::uint32_t pad1 = 0;
	};

	static_assert(sizeof(UiDrawCommand) == 64, "must match DrawCommandData std430 layout");
} // namespace aether::ui
