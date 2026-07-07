#include "FontProcessor.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace fs = std::filesystem;

namespace FontProcessor
{
	namespace
	{
		constexpr std::uint32_t kBakeSizePx = 48;    // FreeType pixel size used to render each glyph.
		constexpr std::uint32_t kAtlasWidthPx = 512; // Fixed shelf-packer width.
		constexpr std::uint32_t kGlyphPaddingPx = 1; // Gutter between glyphs (avoids bilinear-filter bleed).

		// One rasterized glyph, copied out of FreeType's (reused) glyph-slot storage
		// so it survives past the next FT_Load_Glyph call.
		struct RawGlyph
		{
			std::uint32_t codepoint = 0;
			std::uint32_t width = 0;  // bitmap width in px (0 for glyphs with no ink, e.g. space)
			std::uint32_t height = 0; // bitmap height in px
			float bearingX = 0.0f;
			float bearingY = 0.0f;
			float advance = 0.0f;
			std::vector<std::uint8_t> pixels; // width*height, tightly packed row-major (own copy)
		};

		struct PackedRect
		{
			std::uint32_t x = 0;
			std::uint32_t y = 0;
		};

		// RAII wrappers - FreeType is a C API with manual lifetime management, and
		// BakeFont has several early-return error paths.
		struct FaceHandle
		{
			FT_Face face = nullptr;

			~FaceHandle()
			{
				if (face)
				{
					FT_Done_Face(face);
				}
			}
		};

		struct LibraryHandle
		{
			FT_Library library = nullptr;

			~LibraryHandle()
			{
				if (library)
				{
					FT_Done_FreeType(library);
				}
			}
		};

		// Renders one codepoint via FreeType's SDF rasterizer, copying the bitmap out
		// of the glyph slot into an owned buffer. Returns false if the font has no
		// glyph for this codepoint (skip) - space (0x20) is always kept even though
		// some fonts map it to glyph index 0.
		//
		// IMPORTANT: the glyph is loaded WITHOUT FT_LOAD_RENDER. If FT_LOAD_RENDER
		// were used, FreeType would immediately rasterize using the *default* render
		// mode (FT_RENDER_MODE_NORMAL, plain 8-bit coverage), which sets
		// slot->format to FT_GLYPH_FORMAT_BITMAP. A subsequent
		// FT_Render_Glyph(slot, FT_RENDER_MODE_SDF) call on an already-BITMAP slot
		// looks for a renderer registered for BITMAP format (FreeType's "bsdf"
		// module) instead of the outline "sdf" module; for a scalable outline font
		// no such renderer matches, FT_Render_Glyph_Internal treats that as
		// "nothing to do" and returns FT_Err_Ok WITHOUT touching the bitmap - so the
		// atlas would silently end up full of plain antialiased coverage mislabeled
		// as SDF (no error, but wrong: no smooth falloff beyond the glyph edge, and
		// 0.5 would not represent the true edge). Loading with FT_LOAD_DEFAULT keeps
		// slot->format == FT_GLYPH_FORMAT_OUTLINE, so the explicit
		// FT_RENDER_MODE_SDF request below correctly dispatches to the "sdf"
		// renderer.
		bool RasterizeGlyph(FT_Face face, std::uint32_t codepoint, RawGlyph& out)
		{
			const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
			if (glyphIndex == 0 && codepoint != 0x20)
			{
				return false;
			}

			if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != FT_Err_Ok)
			{
				return false;
			}

			if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) != FT_Err_Ok)
			{
				return false;
			}

			const FT_GlyphSlot slot = face->glyph;
			const FT_Bitmap& bitmap = slot->bitmap;

			out.codepoint = codepoint;
			out.width = bitmap.width;
			out.height = bitmap.rows;
			out.bearingX = static_cast<float>(slot->bitmap_left);
			out.bearingY = static_cast<float>(slot->bitmap_top);
			out.advance = static_cast<float>(slot->advance.x >> 6);

			if (out.width > 0 && out.height > 0)
			{
				// pixel_mode is FT_PIXEL_MODE_GRAY for FT_RENDER_MODE_SDF output (same
				// tag as plain 8-bit AA - FreeType distinguishes by renderer, not pixel
				// format). The byte value is FreeType's own SDF encoding: 0 = far
				// outside, ~128 = edge, 255 = far inside - identical to what
				// shaders/text_sdf.slangh expects, so rows are copied verbatim with no
				// inversion or remap.
				out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
				for (std::uint32_t row = 0; row < out.height; ++row)
				{
					// pitch may be negative for a bottom-up bitmap; |pitch| >= width always.
					const unsigned char* srcRow =
					        bitmap.pitch >= 0 ? bitmap.buffer + static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.pitch) : bitmap.buffer + static_cast<std::size_t>(out.height - 1 - row) * static_cast<std::size_t>(-bitmap.pitch);
					std::memcpy(out.pixels.data() + static_cast<std::size_t>(row) * out.width, srcRow, out.width);
				}
			}

			return true;
		}

		bool WriteAtlas(const fs::path& path, std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& pixels)
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				return false;
			}

			const FontAtlasHeader header{kFontAtlasMagic, width, height};
			out.write(reinterpret_cast<const char*>(&header), sizeof(header));
			if (!pixels.empty())
			{
				out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
			}
			return static_cast<bool>(out);
		}

		bool WriteMeta(const fs::path& path, const FontMetaHeader& header, const std::vector<GlyphMeta>& glyphs)
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				return false;
			}

			out.write(reinterpret_cast<const char*>(&header), sizeof(header));
			if (!glyphs.empty())
			{
				out.write(reinterpret_cast<const char*>(glyphs.data()), static_cast<std::streamsize>(glyphs.size() * sizeof(GlyphMeta)));
			}
			return static_cast<bool>(out);
		}

	} // namespace

	BakeResult BakeFont(const fs::path& ttfPath, const fs::path& outDir)
	{
		BakeResult result;

		if (!fs::exists(ttfPath))
		{
			result.error = "font file not found: " + ttfPath.string();
			return result;
		}

		LibraryHandle libraryHandle;
		if (FT_Init_FreeType(&libraryHandle.library) != FT_Err_Ok)
		{
			result.error = "FT_Init_FreeType failed";
			return result;
		}

		FaceHandle faceHandle;
		const std::string ttfPathStr = ttfPath.string();
		if (FT_New_Face(libraryHandle.library, ttfPathStr.c_str(), 0, &faceHandle.face) != FT_Err_Ok)
		{
			result.error = "FT_New_Face failed for '" + ttfPathStr + "'";
			return result;
		}

		if (FT_Set_Pixel_Sizes(faceHandle.face, 0, kBakeSizePx) != FT_Err_Ok)
		{
			result.error = "FT_Set_Pixel_Sizes failed";
			return result;
		}

		std::vector<RawGlyph> glyphs;
		glyphs.reserve(96 + 96);

		// ASCII printable range.
		for (std::uint32_t cp = 0x20; cp <= 0x7E; ++cp)
		{
			RawGlyph g;
			if (RasterizeGlyph(faceHandle.face, cp, g))
			{
				glyphs.push_back(std::move(g));
			}
		}
		// Latin-1 supplement.
		for (std::uint32_t cp = 0xA0; cp <= 0xFF; ++cp)
		{
			RawGlyph g;
			if (RasterizeGlyph(faceHandle.face, cp, g))
			{
				glyphs.push_back(std::move(g));
			}
		}

		if (glyphs.empty())
		{
			result.error = "no glyphs rasterized from '" + ttfPathStr + "'";
			return result;
		}

		// Shelf/row pack: advance the pen by (width + pad); wrap to a new row when
		// it would exceed the fixed atlas width, growing the atlas height as rows
		// accumulate. Zero-size glyphs (e.g. space) don't need atlas space at all.
		std::vector<PackedRect> rects(glyphs.size());
		std::uint32_t penX = 0;
		std::uint32_t penY = 0;
		std::uint32_t rowHeight = 0;
		for (std::size_t i = 0; i < glyphs.size(); ++i)
		{
			const RawGlyph& g = glyphs[i];
			if (g.width == 0)
			{
				rects[i] = {0, 0};
				continue;
			}

			if (penX + g.width > kAtlasWidthPx)
			{
				penX = 0;
				penY += rowHeight + kGlyphPaddingPx;
				rowHeight = 0;
			}

			rects[i] = {penX, penY};
			penX += g.width + kGlyphPaddingPx;
			rowHeight = std::max(rowHeight, g.height);
		}
		const std::uint32_t atlasHeight = penY + rowHeight;

		std::vector<std::uint8_t> atlas(static_cast<std::size_t>(kAtlasWidthPx) * atlasHeight, 0);
		for (std::size_t i = 0; i < glyphs.size(); ++i)
		{
			const RawGlyph& g = glyphs[i];
			if (g.width == 0 || g.height == 0)
			{
				continue;
			}
			for (std::uint32_t row = 0; row < g.height; ++row)
			{
				std::memcpy(atlas.data() + (static_cast<std::size_t>(rects[i].y + row) * kAtlasWidthPx + rects[i].x), g.pixels.data() + static_cast<std::size_t>(row) * g.width, g.width);
			}
		}

		std::vector<GlyphMeta> glyphMetas(glyphs.size());
		for (std::size_t i = 0; i < glyphs.size(); ++i)
		{
			const RawGlyph& g = glyphs[i];
			const PackedRect& r = rects[i];
			GlyphMeta& m = glyphMetas[i];
			m.codepoint = g.codepoint;
			m.u0 = static_cast<float>(r.x) / static_cast<float>(kAtlasWidthPx);
			m.v0 = static_cast<float>(r.y) / static_cast<float>(atlasHeight);
			m.u1 = static_cast<float>(r.x + g.width) / static_cast<float>(kAtlasWidthPx);
			m.v1 = static_cast<float>(r.y + g.height) / static_cast<float>(atlasHeight);
			m.sizeX = static_cast<float>(g.width);
			m.sizeY = static_cast<float>(g.height);
			m.bearingX = g.bearingX;
			m.bearingY = g.bearingY;
			m.advance = g.advance;
		}

		const FT_Size_Metrics& sizeMetrics = faceHandle.face->size->metrics;
		FontMetaHeader metaHeader{};
		metaHeader.magic = kFontMetaMagic;
		metaHeader.version = kFontMetaVersion;
		metaHeader.glyphCount = static_cast<std::uint32_t>(glyphMetas.size());
		metaHeader.atlasWidth = static_cast<float>(kAtlasWidthPx);
		metaHeader.atlasHeight = static_cast<float>(atlasHeight);
		metaHeader.ascent = static_cast<float>(sizeMetrics.ascender >> 6);
		metaHeader.descent = static_cast<float>((-sizeMetrics.descender) >> 6); // descender is <= 0
		metaHeader.lineHeight = static_cast<float>(sizeMetrics.height >> 6);
		metaHeader.bakeSize = static_cast<float>(kBakeSizePx);

		std::error_code ec;
		fs::create_directories(outDir, ec);

		const std::string stem = ttfPath.stem().string();
		const fs::path atlasPath = outDir / (stem + ".fontatlas");
		const fs::path metaPath = outDir / (stem + ".fontmeta");

		if (!WriteAtlas(atlasPath, kAtlasWidthPx, atlasHeight, atlas))
		{
			result.error = "failed to write '" + atlasPath.string() + "'";
			return result;
		}
		if (!WriteMeta(metaPath, metaHeader, glyphMetas))
		{
			result.error = "failed to write '" + metaPath.string() + "'";
			return result;
		}

		result.success = true;
		result.glyphCount = metaHeader.glyphCount;
		result.atlasWidth = kAtlasWidthPx;
		result.atlasHeight = atlasHeight;
		return result;
	}

} // namespace FontProcessor
