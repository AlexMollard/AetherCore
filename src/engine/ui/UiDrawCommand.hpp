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

	// UiDrawCommand::flags layout: bit 0 = pixel-art (nearest) sampling; bit 1 = exclude this command
	// from its owning element's material (build-time only, stripped before upload); bits 8..15 =
	// shaderId, a per-frame index into the material table (0 = default ui_shapes fragment). The id lets
	// the renderer split the one batch into runs drawn with different custom fragment shaders.
	inline constexpr std::uint32_t kFlagPixelArt = 1u;
	// A sub-shape a widget wants drawn on the default pipeline even when the element carries a material
	// (e.g. a toggle's knob, which must stay a crisp sliding dot instead of dissolving into the ink
	// fill). UiDrawBuilder honours this while tagging, then clears the bit so it never reaches the GPU.
	inline constexpr std::uint32_t kFlagNoMaterial = 1u << 1;
	// Clip rect in pixels, honoured only when this bit is set. A flag rather than a sentinel
	// so a zero-size clip legitimately hides content instead of reading as "unclipped".
	inline constexpr std::uint32_t kFlagClip = 1u << 2;
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
		// (x, y, w, h) px. Read by the fragment stage only when kFlagClip is set; a fragment
		// whose SV_Position falls outside is discarded.
		glm::vec4 clipRect{0.f};
		// Field order below is load-bearing: it must stay byte-for-byte with DrawCommandData
		// in shaders/include/UIStructs.slangh (std430).
		std::uint32_t type = kShapeRect;
		std::int32_t layer = 0;
		std::uint32_t textureSlot = 0;
		std::uint32_t flags = 0;
	};

	static_assert(sizeof(UiDrawCommand) == 80, "must match DrawCommandData std430 layout");
} // namespace aether::ui
