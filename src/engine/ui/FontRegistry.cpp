#include "ui/FontRegistry.hpp"

#include <array>
#include <cstring>

#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"

namespace aether::ui
{
	namespace
	{
		// horizontal alignment once the line's total width is known.
		struct LineRange
		{
			std::size_t begin = 0;
			std::size_t end = 0;
			float width = 0.f;
		};
	} // namespace

	std::vector<ShapedGlyph> ShapeText(const FontAsset& font, std::string_view text, float pixelSize, glm::vec4 boxRect, bool wrap, int hAlign, int vAlign)
	{
		std::vector<ShapedGlyph> out;
		if (font.bakeSize <= 0.f || text.empty())
		{
			return out;
		}

		const float scale = pixelSize / font.bakeSize;
		std::vector<LineRange> lines;

		glm::vec2 pen{boxRect.x, boxRect.y};
		int line = 0;
		std::size_t lineStart = 0;

		const auto endLine = [&](float penXAtEnd)
		{
			lines.push_back(LineRange{lineStart, out.size(), penXAtEnd - boxRect.x});
			lineStart = out.size();
		};

		const auto newLine = [&]()
		{
			endLine(pen.x);
			++line;
			pen.x = boxRect.x;
		};

		for (const char ch: text)
		{
			if (ch == '\n')
			{
				newLine();
				continue;
			}

			const auto codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
			const GlyphMeta* glyph = font.Find(codepoint);
			if (glyph == nullptr)
			{
				continue;
			}

			const float advance = glyph->advance * scale;

			// Wrap before placing a glyph that would overflow the box - but never
			if (wrap && pen.x > boxRect.x && pen.x + advance > boxRect.x + boxRect.z)
			{
				newLine();
			}

			const float baseline = boxRect.y + (font.ascent + static_cast<float>(line) * font.lineHeight) * scale;

			ShapedGlyph shaped;
			shaped.rect = {pen.x + glyph->bearingX * scale, baseline - glyph->bearingY * scale, glyph->sizeX * scale, glyph->sizeY * scale};
			shaped.uv = {glyph->u0, glyph->v0, glyph->u1, glyph->v1};
			out.push_back(shaped);

			pen.x += advance;
		}
		endLine(pen.x);

		// Horizontal alignment: shift each line's glyphs by its own slack.
		if (hAlign != 0)
		{
			const float factor = hAlign == 1 ? 0.5f : 1.0f;
			for (const LineRange& lineRange: lines)
			{
				const float shift = (boxRect.z - lineRange.width) * factor;
				for (std::size_t i = lineRange.begin; i < lineRange.end; ++i)
				{
					out[i].rect.x += shift;
				}
			}
		}

		// Vertical alignment: shift the whole shaped block by the box's slack.
		if (vAlign != 0 && !lines.empty())
		{
			// Center/bottom-align the TIGHT glyph box (ascent + descent), not the full
			// line box: lineHeight carries trailing leading below the descent, and
			// including it pushes single-line text visibly high off the box centre.
			const float blockHeight = (static_cast<float>(lines.size() - 1) * font.lineHeight + font.ascent + font.descent) * scale;
			const float factor = vAlign == 1 ? 0.5f : 1.0f;
			const float shift = (boxRect.w - blockHeight) * factor;
			for (ShapedGlyph& shaped: out)
			{
				shaped.rect.y += shift;
			}
		}

		return out;
	}

	const FontAsset* FontRegistry::Load(std::string_view name)
	{
		if (const FontAsset* cached = Get(name))
		{
			return cached;
		}

		// A font name resolves against project fonts first (games bring their
		// own typefaces), then the engine's shipped set; both accept the plain
		// stem and the conventional -Regular suffix.
		const std::array<std::string, 4> candidates{
		        "project://assets/fonts/" + std::string(name) + ".fontmeta",
		        "project://assets/fonts/" + std::string(name) + "-Regular.fontmeta",
		        "engine://fonts/" + std::string(name) + ".fontmeta",
		        "engine://fonts/" + std::string(name) + "-Regular.fontmeta",
		};
		std::string metaPath;
		Expected<std::vector<std::byte>> metaBytes = Unexpected{AetherError::Asset("no font candidates tried")};
		for (const std::string& candidate: candidates)
		{
			metaBytes = io::FileSystem::ReadFile(candidate);
			if (metaBytes.has_value())
			{
				metaPath = candidate;
				break;
			}
		}
		if (!metaBytes.has_value())
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: no .fontmeta for font '{}' in project://assets/fonts or engine://fonts (bake the TTF or check the name)", name);
			return nullptr;
		}

		const std::vector<std::byte>& bytes = *metaBytes;
		if (bytes.size() < sizeof(FontMetaHeader))
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' is too small for a FontMetaHeader", metaPath);
			return nullptr;
		}

		FontMetaHeader header{};
		std::memcpy(&header, bytes.data(), sizeof(FontMetaHeader));

		if (header.magic != kFontMetaMagic)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' has an unrecognised magic - corrupt or not a .fontmeta file", metaPath);
			return nullptr;
		}
		if (header.version != kFontMetaVersion)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' is format version {}, expected {} - re-bake with AssetPacker", metaPath, header.version, kFontMetaVersion);
			return nullptr;
		}

		const std::size_t expectedSize = sizeof(FontMetaHeader) + static_cast<std::size_t>(header.glyphCount) * sizeof(GlyphMeta);
		if (bytes.size() != expectedSize)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' size {} does not match header (expected {} for {} glyphs)", metaPath, bytes.size(), expectedSize, header.glyphCount);
			return nullptr;
		}

		FontAsset asset;
		asset.atlasPath = metaPath.substr(0, metaPath.size() - std::string_view{".fontmeta"}.size()) + ".fontatlas";
		asset.atlasWidth = header.atlasWidth;
		asset.atlasHeight = header.atlasHeight;
		asset.ascent = header.ascent;
		asset.descent = header.descent;
		asset.lineHeight = header.lineHeight;
		asset.bakeSize = header.bakeSize;
		asset.glyphs.reserve(header.glyphCount);

		const std::byte* glyphBytes = bytes.data() + sizeof(FontMetaHeader);
		for (std::uint32_t i = 0; i < header.glyphCount; ++i)
		{
			GlyphMeta glyph{};
			std::memcpy(&glyph, glyphBytes + static_cast<std::size_t>(i) * sizeof(GlyphMeta), sizeof(GlyphMeta));
			asset.glyphs.emplace(glyph.codepoint, glyph);
		}

		const auto [it, inserted] = m_fonts.emplace(std::string(name), std::move(asset));
		return &it->second;
	}

	const FontAsset* FontRegistry::Get(std::string_view name) const
	{
		const auto it = m_fonts.find(std::string(name));
		return it == m_fonts.end() ? nullptr : &it->second;
	}

	FontAsset* FontRegistry::GetMutable(std::string_view name)
	{
		const auto it = m_fonts.find(std::string(name));
		return it == m_fonts.end() ? nullptr : &it->second;
	}

	void FontRegistry::InjectForTest(std::string name, FontAsset asset)
	{
		m_fonts.insert_or_assign(std::move(name), std::move(asset));
	}
} // namespace aether::ui
