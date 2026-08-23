#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// The on-disk font format is declared here (not in the .cpp) so the runtime can mirror it
// from one place - see src/engine/ui/FontAsset.hpp, which must stay byte-for-byte with this.
//
// Fonts are stored as GLYPH OUTLINES, not as a rasterised atlas: quadratic Bezier curves in
// em space plus a per-glyph band acceleration structure, evaluated analytically in the
// fragment shader (the technique Lengyel published as Slug). That is what makes text crisp
// at every size from a single asset - there is no bake size, no atlas, and nothing that
// degrades when a label is drawn at 15px or at 200px.
namespace aether::assetpipeline
{
	namespace FontProcessor
	{
#pragma pack(push, 1)

		// Header of a .fontcurves file. The payload after the glyph table is `texelCount`
		// RGBA32F texels: every glyph's band table followed by its curve list, all indexed
		// by texel number so the runtime uploads it verbatim as one texture.
		struct FontCurveHeader
		{
			std::uint32_t magic;
			std::uint32_t version;
			std::uint32_t glyphCount;
			std::uint32_t texelCount;
			// Texture the runtime must create for the payload. Width is fixed by the baker;
			// height is whatever holds `texelCount`, so a big font simply gets a taller one.
			std::uint32_t textureWidth;
			std::uint32_t textureHeight;
			// Vertical metrics in EM units (1.0 = one em), so a caller multiplies by the
			// pixel size it wants and nothing carries a baked resolution.
			float ascent;
			float descent;
			float lineHeight;
		};

		// One glyph. The outline bounding box IS the quad the renderer draws, which is what
		// lets the shader map its interpolated local coordinate straight onto the curve data
		// without needing the box a second time at runtime.
		struct GlyphCurve
		{
			std::uint32_t codepoint;
			// Outline bounding box in em units, relative to the pen position and baseline.
			float minX, minY, maxX, maxY;
			float advance;
			// Kept explicitly rather than derived from the box so the shaper reads the same
			// field names it always did; bearingX == minX and bearingY == maxY by construction.
			float bearingX, bearingY;
			// Texel index of this glyph's band table: bandCountX horizontal-ray bands first,
			// then bandCountY vertical-ray bands.
			std::uint32_t bandTexel;
			std::uint16_t bandCountX;
			std::uint16_t bandCountY;
		};

#pragma pack(pop)

		static_assert(sizeof(FontCurveHeader) == 36);
		static_assert(sizeof(GlyphCurve) == 40);

		// "AFCV" little-endian FourCC.
		inline constexpr std::uint32_t kFontCurveMagic = (static_cast<std::uint32_t>('A')) | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('C') << 16) | (static_cast<std::uint32_t>('V') << 24);
		inline constexpr std::uint32_t kFontCurveVersion = 1;

		// Texels per row of the curve texture. 1024 keeps even a full Latin-1 face to a few
		// rows while staying far inside any device's 2D image limit.
		inline constexpr std::uint32_t kCurveTextureWidth = 1024;

		// A band entry is one texel; a curve is two (p0.xy, p1.xy) then (p2.xy, 0, 0). Three
		// control points do not divide into a float4, and splitting one curve across a texel
		// boundary would cost the shader an extra fetch and a branch on every curve it tests.
		inline constexpr std::uint32_t kTexelsPerCurve = 2;

		struct BakeResult
		{
			bool success = false;
			std::uint32_t glyphCount = 0;
			std::uint32_t curveCount = 0;
			std::uint32_t textureWidth = 0;
			std::uint32_t textureHeight = 0;
			std::string error;
		};

		[[nodiscard]] BakeResult BakeFont(const std::filesystem::path& ttfPath, const std::filesystem::path& outDir);

	} // namespace FontProcessor
} // namespace aether::assetpipeline
