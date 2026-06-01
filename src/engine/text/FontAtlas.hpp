#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

namespace aether
{
	class BindlessManager;

	// Metrics for a single glyph baked into the atlas.
	struct GlyphInfo
	{
		glm::vec4 uvRect; // (u0, v0, u1, v1) in [0, 1] atlas texture space
		float advanceX;   // Horizontal advance in em units
		float bearingX;   // Left bearing in em units
		float bearingY;   // Top bearing in em units (positive = above baseline)
		float width;      // Glyph bitmap width in em units
		float height;     // Glyph bitmap height in em units
	};

	// Builds a single-channel R8_UNORM signed-distance-field glyph atlas from
	// a TrueType/OpenType font using FreeType's built-in SDF renderer.
	//
	// Covers ASCII 0x20-0x7E (printable characters).
	// Call Build() once and Destroy() when done.
	class FontAtlas
	{
	public:
		static constexpr int kFirstChar = 0x20;
		static constexpr int kLastChar = 0x7E;
		static constexpr int kGlyphCount = kLastChar - kFirstChar + 1;
		static constexpr int kSdfSpread = 8;

		FontAtlas() = default;
		~FontAtlas();

		FontAtlas(const FontAtlas&) = delete;
		FontAtlas& operator=(const FontAtlas&) = delete;

		FontAtlas(FontAtlas&&) noexcept;
		FontAtlas& operator=(FontAtlas&&) noexcept;

		// Load font from the given VFS path (e.g. "assets://fonts/Roboto.ttf")
		// and bake an SDF atlas at `atlasGlyphSize` pixels per glyph cell.
		// Uploads the atlas to the GPU immediately (synchronous).
		void Build(std::string_view fontVfsPath, int atlasGlyphSize, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, uint32_t uploadQueueFamily, BindlessManager& bindless);

		void Destroy();

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] uint32_t GetBindlessSlot() const;

		[[nodiscard]] uint32_t GetAtlasWidth() const
		{
			return m_atlasWidth;
		}

		[[nodiscard]] uint32_t GetAtlasHeight() const
		{
			return m_atlasHeight;
		}

		[[nodiscard]] int GetGlyphSize() const
		{
			return m_glyphSize;
		}

		[[nodiscard]] const GlyphInfo& GetGlyph(char cp) const;

	private:
		VmaAllocator m_allocator = nullptr;
		VkDevice m_device = VK_NULL_HANDLE;
		VkImage m_image = VK_NULL_HANDLE;
		VkImageView m_view = VK_NULL_HANDLE;
		VkSampler m_sampler = VK_NULL_HANDLE;
		VmaAllocation m_allocation = VK_NULL_HANDLE;
		uint32_t m_bindlessSlot = 0xFFFFFFFFu;
		BindlessManager* m_bindlessMgr = nullptr;

		uint32_t m_atlasWidth = 0;
		uint32_t m_atlasHeight = 0;
		int m_glyphSize = 0;

		std::array<GlyphInfo, kGlyphCount> m_glyphs{};
		GlyphInfo m_fallbackGlyph{};
	};
} // namespace aether
