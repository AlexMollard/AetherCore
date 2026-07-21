#pragma once

#include <cstdint>
#include <string>
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

	// UiDrawCommand::flags layout: bit 0 = pixel-art (nearest) sampling; bits 8..15 = shaderId,
	// a per-frame index into the material table (0 = default ui_shapes fragment). The id lets the
	// renderer split the one batch into runs drawn with different custom fragment shaders.
	inline constexpr std::uint32_t kFlagPixelArt = 1u;
	inline constexpr std::uint32_t kShaderIdShift = 8u;
	inline constexpr std::uint32_t kShaderIdMask = 0xFFu;

	inline std::uint32_t UiFlagsShaderId(std::uint32_t flags)
	{
		return (flags >> kShaderIdShift) & kShaderIdMask;
	}

	inline std::uint32_t UiFlagsWithShaderId(std::uint32_t flags, std::uint32_t shaderId)
	{
		return (flags & ~(kShaderIdMask << kShaderIdShift)) | ((shaderId & kShaderIdMask) << kShaderIdShift);
	}

	// One entry per material-tagged element this frame; index+1 is the command's shaderId.
	// Carries what the renderer needs to bind the custom pipeline and push its effect params.
	struct UiMaterialDraw
	{
		std::string shader;
		glm::vec4 params{0.f};
		glm::vec4 color0{0.f};
		glm::vec4 color1{0.f};
	};

	struct UiDrawCommand
	{
		glm::vec4 data0{0.f};
		glm::vec4 data1{0.f};
		glm::vec4 color{1.f};
		std::uint32_t type = kShapeRect;
		std::int32_t layer = 0;
		std::uint32_t textureSlot = 0;
		std::uint32_t flags = 0; // bit 0 = pixel-art (nearest) sampling
	};

	static_assert(sizeof(UiDrawCommand) == 64, "must match DrawCommandData std430 layout");
} // namespace aether::ui
