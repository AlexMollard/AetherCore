#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Both on-disk structs are declared here (not in the .cpp) so the runtime
namespace aether::assetpipeline
{
	namespace FontProcessor
	{
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

		struct GlyphMeta
		{
			std::uint32_t codepoint;
			float u0, v0, u1, v1;
			float sizeX, sizeY;
			float bearingX, bearingY;
			float advance;
		};

#pragma pack(pop)

		static_assert(sizeof(FontAtlasHeader) == 12);
		static_assert(sizeof(FontMetaHeader) == 36);
		static_assert(sizeof(GlyphMeta) == 40);

		inline constexpr std::uint32_t kFontAtlasMagic = (static_cast<std::uint32_t>('A')) | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('N') << 16) | (static_cast<std::uint32_t>('T') << 24);
		inline constexpr std::uint32_t kFontMetaMagic = (static_cast<std::uint32_t>('A')) | (static_cast<std::uint32_t>('F') << 8) | (static_cast<std::uint32_t>('M') << 16) | (static_cast<std::uint32_t>('T') << 24);
		inline constexpr std::uint32_t kFontMetaVersion = 2;

		struct BakeResult
		{
			bool success = false;
			std::uint32_t glyphCount = 0;
			std::uint32_t atlasWidth = 0;
			std::uint32_t atlasHeight = 0;
			std::string error;
		};

		[[nodiscard]] BakeResult BakeFont(const std::filesystem::path& ttfPath, const std::filesystem::path& outDir);

	} // namespace FontProcessor
} // namespace aether::assetpipeline
