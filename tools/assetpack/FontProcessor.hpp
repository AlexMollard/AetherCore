#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Offline SDF font baker: turns a .ttf into two loose files written next to
// each other (same stem, different extension) so the existing
// PakWriter.AddDirectory bundles them as ordinary files:
//
//   <name>.fontatlas - FontAtlasHeader + width*height bytes of R8_UNORM SDF.
//   <name>.fontmeta  - FontMetaHeader  + glyphCount * GlyphMeta entries.
//
// Both on-disk structs are declared here (not in the .cpp) so the runtime
// font-loading task can #include this header directly and reinterpret the
// file bytes without redeclaring the layout.
namespace aether::assetpipeline
{
	namespace FontProcessor
	{
		// ── <name>.fontatlas ─────────────────────────────────────────────────────
		//
		// Header immediately followed by width*height bytes of single-channel
		// (R8_UNORM) data, row-major, top row first. Matches shaders/text_sdf.slangh:
		// 0 = far outside, ~128/255 (0.5) = glyph edge, 255 = far inside. This is
		// FreeType's own FT_RENDER_MODE_SDF byte encoding copied verbatim - no
		// inversion or remap needed when baking or when sampling at runtime.
#pragma pack(push, 1)

		struct FontAtlasHeader
		{
			std::uint32_t magic;
			std::uint32_t width;
			std::uint32_t height;
		};

		// ── <name>.fontmeta ──────────────────────────────────────────────────────
		//
		// Header immediately followed by glyphCount * GlyphMeta entries, sorted by
		// ascending codepoint.
		struct FontMetaHeader
		{
			std::uint32_t magic;
			std::uint32_t version;
			std::uint32_t glyphCount;
			float atlasWidth;
			float atlasHeight;
			float ascent;
			float descent;
			float lineHeight;
			float bakeSize; // FreeType pixel size the glyphs were rasterized at (kBakeSizePx); runtime scales by pixelSize/bakeSize.
		};

		struct GlyphMeta
		{
			std::uint32_t codepoint;
			float u0, v0, u1, v1;     // UV rect in [0,1]
			float sizeX, sizeY;       // glyph bitmap size in px
			float bearingX, bearingY; // left/top bearing in px
			float advance;            // horizontal advance in px
		};

#pragma pack(pop)

		static_assert(sizeof(FontAtlasHeader) == 12);
		static_assert(sizeof(FontMetaHeader) == 36);
		static_assert(sizeof(GlyphMeta) == 40);

		// "AFNT" / "AFMT" little-endian FourCC tags (first character = low byte),
		// same convention as DDS_MAGIC in DDSFormat.hpp.
		inline constexpr std::uint32_t kFontAtlasMagic = (static_cast<std::uint32_t>('A')) | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('N') << 16) | (static_cast<std::uint32_t>('T') << 24);
		inline constexpr std::uint32_t kFontMetaMagic = (static_cast<std::uint32_t>('A')) | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('M') << 16) | (static_cast<std::uint32_t>('T') << 24);
		inline constexpr std::uint32_t kFontMetaVersion = 2;

		// Result of a bake - carries enough information for a caller-side self-check
		// without needing to re-open the files it just wrote.
		struct BakeResult
		{
			bool success = false;
			std::uint32_t glyphCount = 0;
			std::uint32_t atlasWidth = 0;
			std::uint32_t atlasHeight = 0;
			std::string error; // human-readable failure reason, set iff !success
		};

		// Bakes ttfPath into "<outDir>/<ttfPath.stem()>.fontatlas" and ".fontmeta".
		//
		// Rasterizes ASCII 0x20-0x7E and Latin-1 0xA0-0xFF via FreeType's SDF
		// rasterizer (FT_RENDER_MODE_SDF; requires FreeType >= 2.11) into a single
		// shelf-packed atlas (fixed width, height grows to fit). Codepoints with no
		// glyph in the font are skipped, except space which is always kept.
		[[nodiscard]] BakeResult BakeFont(const std::filesystem::path& ttfPath, const std::filesystem::path& outDir);

	} // namespace FontProcessor
} // namespace aether::assetpipeline
