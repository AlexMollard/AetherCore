#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string_view>

#include "gpu/GpuTypes.hpp"
#include "gpu/GpuHandles.hpp"

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
	//
	// The atlas is uploaded to the GPU via gpu::ResourceRegistry +
	// gpu::OneShotCmd; the R8_UNORM channel is expanded to RGBA8 in the
	// bindless descriptor via a (R, 0, 0, 1) component swizzle on the
	// registry-created view.
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
		// All GPU primitives are passed as opaque `gpu::` types; the call
		// site (TextRenderer) is the only place that resolves them from
		// the engine-side GpuDevice accessors.
		void Build(std::string_view fontVfsPath, int atlasGlyphSize, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, std::uint32_t uploadQueueFamily, BindlessManager& bindless);

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
		gpu::TextureHandle m_atlasHandle{};
		gpu::ImageView m_view = nullptr;
		// Upload context is rebuilt on every Build() and torn down on
		// Destroy(). The pool is owned by the factory (not the registry)
		// so it is destroyed immediately when no longer needed.
		gpu::Device m_device = nullptr;
		gpu::CommandPool m_uploadPool = nullptr;
		std::uint32_t m_bindlessSlot = 0xFFFFFFFFu;
		BindlessManager* m_bindlessMgr = nullptr;
		std::uint32_t m_atlasWidth = 0;
		std::uint32_t m_atlasHeight = 0;
		int m_glyphSize = 0;

		std::array<GlyphInfo, kGlyphCount> m_glyphs{};
		GlyphInfo m_fallbackGlyph{};
	};
} // namespace aether
