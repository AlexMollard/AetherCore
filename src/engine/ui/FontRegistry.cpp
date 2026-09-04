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
		if (pixelSize <= 0.f || text.empty())
		{
			return out;
		}

		// Metrics are in em units, so the requested pixel size IS the scale. There is no bake
		// size to divide by, and nothing degrades when a label asks for a size no one baked.
		const float scale = pixelSize;
		std::vector<LineRange> lines;

		glm::vec2 pen{boxRect.x, boxRect.y};
		int line = 0;
		std::size_t lineStart = 0;
		// Where the word being built started, so a wrap can carry the whole of it down
		// instead of guillotining it at whichever glyph happened to cross the edge.
		std::size_t wordStart = 0;
		float wordStartPen = boxRect.x;

		const auto endLineAt = [&](std::size_t endIndex, float penXAtEnd)
		{
			lines.push_back(LineRange{lineStart, endIndex, penXAtEnd - boxRect.x});
			lineStart = endIndex;
		};

		const auto newLine = [&]()
		{
			endLineAt(out.size(), pen.x);
			++line;
			pen.x = boxRect.x;
			wordStart = out.size();
			wordStartPen = pen.x;
		};

		// Move the partially-typed word at the end of the current line down onto the next
		// one. Text wraps between WORDS - breaking inside one is the difference between a
		// paragraph and a ransom note - and the glyphs are already placed, so carrying them
		// is a shift of both axes rather than a re-layout.
		const auto carryWordToNextLine = [&]()
		{
			const float dx = boxRect.x - wordStartPen;
			const float dy = font.lineHeight * scale;
			endLineAt(wordStart, wordStartPen);
			for (std::size_t i = wordStart; i < out.size(); ++i)
			{
				out[i].rect.x += dx;
				out[i].rect.y += dy;
			}
			++line;
			pen.x += dx;
			wordStartPen = boxRect.x;
		};

		for (const char ch: text)
		{
			if (ch == '\n')
			{
				newLine();
				continue;
			}

			const auto codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
			const GlyphCurve* glyph = font.Find(codepoint);
			if (glyph == nullptr)
			{
				continue;
			}

			const float advance = glyph->advance * scale;

			// Wrap before placing a glyph that would overflow the box - but never on the
			// first glyph of a line, or a box narrower than one character would loop forever.
			if (wrap && pen.x > boxRect.x && pen.x + advance > boxRect.x + boxRect.z)
			{
				// Carry the word down whole when there is something before it on this line to
				// keep. A single word wider than the whole box has nowhere to go, so that one
				// still breaks where it overflows - which is the only case that should.
				if (wordStart > lineStart)
				{
					carryWordToNextLine();
				}
				else
				{
					newLine();
				}
			}

			const float baseline = boxRect.y + (font.ascent + static_cast<float>(line) * font.lineHeight) * scale;

			// A blank glyph (a space) has no outline: it advances the pen and emits nothing,
			// so no degenerate quad ever reaches the shader.
			if (glyph->bandCountX > 0)
			{
				ShapedGlyph shaped;
				// The quad IS the outline box, which is what lets the shader read its own
				// interpolated local coordinate as a position in the glyph's curve space.
				shaped.rect = {
				        pen.x + glyph->bearingX * scale,
				        baseline - glyph->bearingY * scale,
				        (glyph->maxX - glyph->minX) * scale,
				        (glyph->maxY - glyph->minY) * scale};
				shaped.bands = {
				        static_cast<float>(glyph->bandTexel),
				        static_cast<float>(glyph->bandCountX),
				        static_cast<float>(glyph->bandCountY),
				        0.f};
				out.push_back(shaped);
			}

			pen.x += advance;

			// A space ends the word: everything after it is the next candidate to carry.
			if (ch == ' ')
			{
				wordStart = out.size();
				wordStartPen = pen.x;
			}
		}
		// A trailing '\n' already closed the last real line inside the loop; closing again
		// here would append an empty LineRange and inflate the vertical-alignment block
		// height by a full lineHeight. An empty final segment (only spaces or missing
		// glyphs since the last newline) has nothing on it to align, so it is not a line.
		if (out.size() != lineStart)
		{
			endLineAt(out.size(), pen.x);
		}

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
		        "project://assets/fonts/" + std::string(name) + ".fontcurves",
		        "project://assets/fonts/" + std::string(name) + "-Regular.fontcurves",
		        "engine://fonts/" + std::string(name) + ".fontcurves",
		        "engine://fonts/" + std::string(name) + "-Regular.fontcurves",
		};
		std::string curvePath;
		Expected<std::vector<std::byte>> bytesResult = Unexpected{AetherError::Asset("no font candidates tried")};
		for (const std::string& candidate: candidates)
		{
			bytesResult = io::FileSystem::ReadFile(candidate);
			if (bytesResult.has_value())
			{
				curvePath = candidate;
				break;
			}
		}
		if (!bytesResult.has_value())
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: no .fontcurves for font '{}' in project://assets/fonts or engine://fonts (bake the TTF or check the name)", name);
			return nullptr;
		}

		const std::vector<std::byte>& bytes = *bytesResult;
		if (bytes.size() < sizeof(FontCurveHeader))
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' is too small for a FontCurveHeader", curvePath);
			return nullptr;
		}

		FontCurveHeader header{};
		std::memcpy(&header, bytes.data(), sizeof(FontCurveHeader));

		if (header.magic != kFontCurveMagic)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' has an unrecognised magic - corrupt or not a .fontcurves file", curvePath);
			return nullptr;
		}
		if (header.version != kFontCurveVersion)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' is format version {}, expected {} - re-bake with AssetPacker", curvePath, header.version, kFontCurveVersion);
			return nullptr;
		}

		const std::size_t glyphBytes = static_cast<std::size_t>(header.glyphCount) * sizeof(GlyphCurve);
		const std::size_t texelBytes = static_cast<std::size_t>(header.texelCount) * sizeof(glm::vec4);
		const std::size_t expectedSize = sizeof(FontCurveHeader) + glyphBytes + texelBytes;
		if (bytes.size() != expectedSize)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' size {} does not match its header (expected {} for {} glyphs and {} texels)", curvePath, bytes.size(), expectedSize, header.glyphCount, header.texelCount);
			return nullptr;
		}
		if (static_cast<std::size_t>(header.textureWidth) * header.textureHeight != header.texelCount)
		{
			AE_ERROR(LogCategory::UI, "FontRegistry: '{}' declares a {}x{} curve texture that does not hold its {} texels", curvePath, header.textureWidth, header.textureHeight, header.texelCount);
			return nullptr;
		}

		FontAsset asset;
		asset.curvePath = curvePath;
		asset.textureWidth = header.textureWidth;
		asset.textureHeight = header.textureHeight;
		asset.ascent = header.ascent;
		asset.descent = header.descent;
		asset.lineHeight = header.lineHeight;
		asset.glyphs.reserve(header.glyphCount);

		const std::byte* glyphBase = bytes.data() + sizeof(FontCurveHeader);
		for (std::uint32_t i = 0; i < header.glyphCount; ++i)
		{
			GlyphCurve glyph{};
			std::memcpy(&glyph, glyphBase + static_cast<std::size_t>(i) * sizeof(GlyphCurve), sizeof(GlyphCurve));
			asset.glyphs.emplace(glyph.codepoint, glyph);
		}

		asset.texels.resize(header.texelCount);
		if (header.texelCount > 0)
		{
			std::memcpy(asset.texels.data(), glyphBase + glyphBytes, texelBytes);
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
