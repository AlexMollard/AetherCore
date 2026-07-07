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

	// Mirrors DrawCommandData in shaders/include/UIStructs.slangh. Uploaded verbatim
	// to a device-address buffer the ui_shapes shader reads by instance index.
	struct UiDrawCommand
	{
		glm::vec4 data0{0.f}; // Rect: x,y,w,h | Circle: cx,cy,r,0 | Line: x0,y0,x1,y1
		glm::vec4 data1{0.f}; // Rect/Line: cornerRadius/thickness | TexturedRect: u0,v0,u1,v1
		glm::vec4 color{1.f}; // RGBA tint
		std::uint32_t type = kShapeRect;
		std::int32_t layer = 0;
		std::uint32_t textureSlot = 0; // bindless slot (TexturedRect only)
		std::uint32_t pad1 = 0;
	};

	static_assert(sizeof(UiDrawCommand) == 64, "must match DrawCommandData std430 layout");
} // namespace aether::ui
