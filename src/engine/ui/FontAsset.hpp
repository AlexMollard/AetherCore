#pragma once

#include <cstdint>
#include <unordered_map>

#include <glm/glm.hpp>

namespace aether::ui
{
	// On-disk glyph record - MUST match FontProcessor::GlyphMeta byte-for-byte
#pragma pack(push, 1)

	struct GlyphMeta
	{
		std::uint32_t codepoint;
		float u0, v0, u1, v1;
		float sizeX, sizeY;
		float bearingX, bearingY;
		float advance;
	};

#pragma pack(pop)

	static_assert(sizeof(GlyphMeta) == 40, "must match FontProcessor::GlyphMeta - runtime/baker format drift");

	// On-disk headers - MUST match FontProcessor::FontAtlasHeader / FontMetaHeader
#pragma pack(push, 1)

	struct FontAtlasHeader
	{
		std::uint32_t magic;
		std::uint32_t width;
		std::uint32_t height;
	};

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
		float bakeSize;
	};

#pragma pack(pop)

	static_assert(sizeof(FontAtlasHeader) == 12, "must match FontProcessor::FontAtlasHeader");
	static_assert(sizeof(FontMetaHeader) == 36, "must match FontProcessor::FontMetaHeader");

	// "AFNT"/"AFMT" little-endian FourCC + format version - must match FontProcessor.hpp.
	inline constexpr std::uint32_t kFontAtlasMagic = static_cast<std::uint32_t>('A') | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('N') << 16) | (static_cast<std::uint32_t>('T') << 24);
	inline constexpr std::uint32_t kFontMetaMagic = static_cast<std::uint32_t>('A') | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('M') << 16) | (static_cast<std::uint32_t>('T') << 24);
	inline constexpr std::uint32_t kFontMetaVersion = 2;

	// tests that never touch the GPU).
	struct FontAsset
	{
		std::uint32_t atlasBindlessSlot = 0xFFFFFFFFu;
		float atlasWidth = 0.f, atlasHeight = 0.f;
		float ascent = 0.f, descent = 0.f, lineHeight = 0.f, bakeSize = 48.f;
		std::unordered_map<std::uint32_t, GlyphMeta> glyphs;

		[[nodiscard]] const GlyphMeta* Find(std::uint32_t cp) const
		{
			const auto it = glyphs.find(cp);
			return it == glyphs.end() ? nullptr : &it->second;
		}
	};

	struct ShapedGlyph
	{
		glm::vec4 rect;
		glm::vec4 uv;
	};
} // namespace aether::ui
