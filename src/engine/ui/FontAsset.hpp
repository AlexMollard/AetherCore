#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace aether::ui
{
	// A font is GLYPH OUTLINES, not a rasterised atlas: quadratic Bezier curves in em space
	// plus a per-glyph band acceleration structure, evaluated analytically by the fragment
	// shader. There is no bake size and no atlas, so one asset is crisp at 12px and at 200px.
	//
	// On-disk records - MUST match tools/assetpack/FontProcessor.hpp byte-for-byte.
#pragma pack(push, 1)

	struct FontCurveHeader
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t glyphCount;
		std::uint32_t texelCount;
		std::uint32_t textureWidth;
		std::uint32_t textureHeight;
		float ascent;
		float descent;
		float lineHeight;
	};

	struct GlyphCurve
	{
		std::uint32_t codepoint;
		// Outline bounding box in em units. This box IS the quad the renderer draws, which is
		// what lets the shader map its local coordinate onto the curve data with no extra
		// per-glyph constants.
		float minX, minY, maxX, maxY;
		float advance;
		float bearingX, bearingY;
		// Texel index of this glyph's band table: bandCountX horizontal-ray bands, then
		// bandCountY vertical-ray bands.
		std::uint32_t bandTexel;
		std::uint16_t bandCountX;
		std::uint16_t bandCountY;
	};

#pragma pack(pop)

	static_assert(sizeof(FontCurveHeader) == 36, "must match FontProcessor::FontCurveHeader");
	static_assert(sizeof(GlyphCurve) == 40, "must match FontProcessor::GlyphCurve");

	// "AFCV" little-endian FourCC + format version - must match FontProcessor.hpp.
	inline constexpr std::uint32_t kFontCurveMagic = static_cast<std::uint32_t>('A') | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('C') << 16) | (static_cast<std::uint32_t>('V') << 24);
	inline constexpr std::uint32_t kFontCurveVersion = 1;

	struct FontAsset
	{
		// Bindless slot of this font's curve texture (RGBA32F). One texture holds every
		// glyph's band table and curve runs; a glyph addresses into it by texel index.
		std::uint32_t curveBindlessSlot = 0xFFFFFFFFu;
		std::uint32_t textureWidth = 0, textureHeight = 0;
		// Vertical metrics in EM units, so a caller multiplies by the pixel size it wants.
		float ascent = 0.f, descent = 0.f, lineHeight = 0.f;
		// VFS path of the .fontcurves this came from, recorded when it loads so the renderer
		// never re-derives it from the font name.
		std::string curvePath;
		// The payload, kept CPU-side until the renderer uploads it; cleared afterwards so a
		// loaded font costs a map of metrics and nothing else.
		std::vector<glm::vec4> texels;
		std::unordered_map<std::uint32_t, GlyphCurve> glyphs;

		[[nodiscard]] const GlyphCurve* Find(std::uint32_t cp) const
		{
			const auto it = glyphs.find(cp);
			return it == glyphs.end() ? nullptr : &it->second;
		}
	};

	struct ShapedGlyph
	{
		// Screen-space quad: the glyph's outline box, in pixels.
		glm::vec4 rect;
		// Where the shader finds this glyph's curves: (bandTexel, bandCountX, bandCountY, 0).
		// Carried in the same draw-command slot the atlas UV rect used to occupy.
		glm::vec4 bands;
	};
} // namespace aether::ui
