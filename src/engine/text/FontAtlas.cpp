#include "text/FontAtlas.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <ft2build.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/OneShotCmd.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/FileSystem.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"

namespace aether
{
	// -- Move semantics --------------------------------------------------------

	FontAtlas::FontAtlas(FontAtlas&& o) noexcept
	      : m_atlasHandle(std::exchange(o.m_atlasHandle, {})),
	        m_view(std::exchange(o.m_view, nullptr)),
	        m_sampler(std::exchange(o.m_sampler, nullptr)),
	        m_device(std::exchange(o.m_device, nullptr)),
	        m_uploadPool(std::exchange(o.m_uploadPool, nullptr)),
	        m_bindlessSlot(std::exchange(o.m_bindlessSlot, 0xFFFFFFFFu)),
	        m_bindlessMgr(std::exchange(o.m_bindlessMgr, nullptr)),
	        m_atlasWidth(std::exchange(o.m_atlasWidth, 0u)),
	        m_atlasHeight(std::exchange(o.m_atlasHeight, 0u)),
	        m_glyphSize(std::exchange(o.m_glyphSize, 0)),
	        m_glyphs(o.m_glyphs)
	{
	}

	FontAtlas& FontAtlas::operator=(FontAtlas&& o) noexcept
	{
		if (this != &o)
		{
			Destroy();
			m_atlasHandle = std::exchange(o.m_atlasHandle, {});
			m_view = std::exchange(o.m_view, nullptr);
			m_sampler = std::exchange(o.m_sampler, nullptr);
			m_device = std::exchange(o.m_device, nullptr);
			m_uploadPool = std::exchange(o.m_uploadPool, nullptr);
			m_bindlessSlot = std::exchange(o.m_bindlessSlot, 0xFFFFFFFFu);
			m_bindlessMgr = std::exchange(o.m_bindlessMgr, nullptr);
			m_atlasWidth = std::exchange(o.m_atlasWidth, 0u);
			m_atlasHeight = std::exchange(o.m_atlasHeight, 0u);
			m_glyphSize = std::exchange(o.m_glyphSize, 0);
			m_glyphs = o.m_glyphs;
		}
		return *this;
	}

	FontAtlas::~FontAtlas()
	{
		Destroy();
	}

	// -- Build -----------------------------------------------------------------

	void FontAtlas::Build(std::string_view fontVfsPath, int atlasGlyphSize, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, std::uint32_t uploadQueueFamily, BindlessManager& bindless)
	{
		(void) allocator;
		m_bindlessMgr = &bindless;
		m_device = device;

		// -- 1. Initialise FreeType --------------------------------------------
		FT_Library ft{};
		if (FT_Init_FreeType(&ft) != 0)
		{
			Throw(AetherError::Engine("FontAtlas: FT_Init_FreeType failed."));
		}

		AE_EXPECT_OR_THROW(fontData, io::FileSystem::ReadFile(fontVfsPath));

		FT_Face face{};
		if (FT_New_Memory_Face(ft, reinterpret_cast<const FT_Byte*>(fontData.data()), static_cast<FT_Long>(fontData.size()), 0, &face) != 0)
		{
			FT_Done_FreeType(ft);
			Throw(AetherError::Asset(std::string("FontAtlas: FT_New_Memory_Face failed for '") + std::string(fontVfsPath) + "'."));
		}

		FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(atlasGlyphSize));

		auto spread = static_cast<FT_UInt>(kSdfSpread);
		FT_Property_Set(ft, "sdf", "spread", &spread);

		// -- 2. Render all glyphs into a CPU-side atlas ------------------------
		const int cellSize = atlasGlyphSize + 2 * kSdfSpread;

		const int cols = static_cast<int>(std::sqrt(static_cast<double>(kGlyphCount))) + 1;
		const int rows = (kGlyphCount + cols - 1) / cols;

		auto nextPow2 = [](int v)
		{
			int p = 1;
			while (p < v)
			{
				p <<= 1;
			}
			return p;
		};

		const auto atlasW = static_cast<uint32_t>(nextPow2(cols * cellSize));
		const auto atlasH = static_cast<uint32_t>(nextPow2(rows * cellSize));

		std::vector<std::uint8_t> atlasPixels(static_cast<size_t>(atlasW * atlasH), 0u);

		const auto fAtlasW = static_cast<float>(atlasW);
		const auto fAtlasH = static_cast<float>(atlasH);

		for (int i = 0; i < kGlyphCount; ++i)
		{
			const char cp = static_cast<char>(kFirstChar + i);
			const auto charcode = static_cast<FT_ULong>(static_cast<unsigned char>(cp));
			const FT_UInt glyphIdx = FT_Get_Char_Index(face, charcode);

			GlyphInfo& info = m_glyphs[i];

			if (glyphIdx == 0)
			{
				info = {};
				continue;
			}

			FT_Error loadErr = FT_Load_Glyph(face, glyphIdx, FT_LOAD_DEFAULT);
			if (loadErr != 0)
			{
				info = {};
				continue;
			}

			FT_Error renderErr = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF);
			if (renderErr != 0)
			{
				renderErr = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
			}
			if (renderErr != 0)
			{
				info = {};
				continue;
			}

			const FT_Bitmap& bm = face->glyph->bitmap;

			const int col = i % cols;
			const int row = i / cols;
			const int origX = col * cellSize;
			const int origY = row * cellSize;

			for (unsigned int by = 0; by < bm.rows; ++by)
			{
				const int dstY = origY + static_cast<int>(by);
				if (dstY < 0 || std::cmp_greater_equal(dstY, atlasH))
				{
					continue;
				}

				for (unsigned int bx = 0; bx < bm.width; ++bx)
				{
					const int dstX = origX + static_cast<int>(bx);
					if (dstX < 0 || std::cmp_greater_equal(dstX, atlasW))
					{
						continue;
					}

					atlasPixels[static_cast<std::size_t>(dstY) * atlasW + dstX] = bm.buffer[by * static_cast<unsigned>(std::abs(bm.pitch)) + bx];
				}
			}

			const float emScale = 1.0f / static_cast<float>(atlasGlyphSize);
			info.uvRect = glm::vec4(static_cast<float>(origX) / fAtlasW, static_cast<float>(origY) / fAtlasH, static_cast<float>(origX + static_cast<int>(bm.width)) / fAtlasW, static_cast<float>(origY + static_cast<int>(bm.rows)) / fAtlasH);
			info.advanceX = static_cast<float>(face->glyph->advance.x >> 6) * emScale;
			info.bearingX = static_cast<float>(face->glyph->metrics.horiBearingX >> 6) * emScale;
			info.bearingY = static_cast<float>(face->glyph->metrics.horiBearingY >> 6) * emScale;
			info.width = static_cast<float>(bm.width) * emScale;
			info.height = static_cast<float>(bm.rows) * emScale;
		}

		FT_Done_Face(face);
		FT_Done_FreeType(ft);

		m_glyphSize = atlasGlyphSize;
		m_atlasWidth = atlasW;
		m_atlasHeight = atlasH;

		// -- 3. Create the atlas texture via the registry ---------------------
		// The R8_UNORM channel is expanded to RGBA8 in the bindless
		// descriptor via a (R, 0, 0, 1) component swizzle on the
		// registry-created view (see gpu::ComponentSwizzle). The shader
		// reads .rgba as if the format were RGBA8.
		const gpu::TextureDesc atlasDesc{
		        .format = gpu::Format::R8Unorm,
		        .extent = {atlasW, atlasH},
		        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .r = gpu::ComponentSwizzle::R,
		        .g = gpu::ComponentSwizzle::Zero,
		        .b = gpu::ComponentSwizzle::Zero,
		        .a = gpu::ComponentSwizzle::One,
		        .debugName = "FontAtlas",
		};
		m_atlasHandle = gpu::ResourceRegistry::CreateTexture(atlasDesc);
		if (!m_atlasHandle.IsValid())
		{
			Throw(AetherError::Engine("FontAtlas: CreateTexture failed"));
		}

		// -- 4. Host copy: synchronously writes the SDF atlas pixels --------
		{
			const std::int32_t copyResult = gpu::Factory::HostCopyToImage(device, gpu::ResourceRegistry::ResolveTextureImage(m_atlasHandle), atlasPixels.data(), atlasW, atlasH);
			if (copyResult != 0)
			{
				gpu::ResourceRegistry::Destroy(m_atlasHandle);
				m_atlasHandle = {};
				Throw(AetherError::Vulkan(copyResult, "FontAtlas: HostCopyToImage failed"));
			}
		}

		// -- 5. Resolve the swizzled view (registry created it) -------------
		m_view = gpu::ResourceRegistry::ResolveTexture(m_atlasHandle).view;

		// -- 6. Acquire a linear/clamp sampler from the bindless cache -------
		AE_EXPECT_OR_THROW(samplerResult, bindless.GetOrCreateSampler(gpu::Filter::Linear, gpu::SamplerMipmapMode::Linear, gpu::SamplerAddressMode::ClampToEdge));
		m_sampler = samplerResult;

		// -- 7. Layout transition via the one-shot upload path --------------
		m_uploadPool = gpu::Factory::CreateCommandPool(device,
		        gpu::Factory::CommandPoolDesc{
		                .queueFamilyIndex = uploadQueueFamily,
		                .transient = true,
		        });
		if (m_uploadPool == nullptr)
		{
			gpu::ResourceRegistry::Destroy(m_atlasHandle);
			m_atlasHandle = {};
			Throw(AetherError::Engine("FontAtlas: CreateCommandPool failed"));
		}

		{
			gpu::OneShotCmd cmd;
			if (!cmd.Begin(device, m_uploadPool))
			{
				gpu::Factory::DestroyCommandPool(device, m_uploadPool);
				m_uploadPool = nullptr;
				gpu::ResourceRegistry::Destroy(m_atlasHandle);
				m_atlasHandle = {};
				Throw(AetherError::Engine("FontAtlas: OneShotCmd::Begin failed"));
			}
			cmd.CmdList().ImageMemoryBarrier(gpu::ResourceRegistry::ResolveTextureImage(m_atlasHandle),
			        gpu::ImageLayout::General,
			        gpu::ImageLayout::ShaderReadOnly,
			        gpu::ImageAspect::Color,
			        gpu::PipelineStage::AllCommands,
			        gpu::AccessFlags::None,
			        gpu::PipelineStage::FragmentShader,
			        gpu::AccessFlags::ShaderRead);
			if (!cmd.EndAndSubmit(uploadQueue))
			{
				gpu::Factory::DestroyCommandPool(device, m_uploadPool);
				m_uploadPool = nullptr;
				gpu::ResourceRegistry::Destroy(m_atlasHandle);
				m_atlasHandle = {};
				Throw(AetherError::Engine("FontAtlas: OneShotCmd::EndAndSubmit failed"));
			}
		}

		// Upload pool is single-use; tear it down eagerly. The image /
		// view / sampler lifetime is owned by the registry / bindless
		// cache, so they outlive Destroy() until registry.AdvanceFrame
		// and bindless cache eviction.
		gpu::Factory::DestroyCommandPool(device, m_uploadPool);
		m_uploadPool = nullptr;

		// -- 8. Register in the bindless descriptor set --------------------
		AE_EXPECT_OR_THROW(slotResult, bindless.AllocateSampledImageSlot());
		m_bindlessSlot = slotResult;
		const Expected<void> updateResult = bindless.UpdateSampledImage(m_bindlessSlot, m_view, m_sampler, gpu::ImageLayout::ShaderReadOnly);
		if (!updateResult)
		{
			bindless.FreeSampledImageSlot(m_bindlessSlot);
			m_bindlessSlot = 0xFFFFFFFFu;
			Throw(updateResult.error());
		}

		AE_INFO(LogCategory::Asset, "FontAtlas built: {} glyphs, atlas {}x{}, bindless slot {}.", kGlyphCount, atlasW, atlasH, m_bindlessSlot);
	}

	// -- Destroy ---------------------------------------------------------------

	void FontAtlas::Destroy()
	{
		if (m_device == nullptr)
		{
			return;
		}

		if (m_bindlessSlot != 0xFFFFFFFFu && m_bindlessMgr != nullptr)
		{
			m_bindlessMgr->FreeSampledImageSlot(m_bindlessSlot);
			m_bindlessSlot = 0xFFFFFFFFu;
		}
		// m_sampler is owned by BindlessManager's cache; no destroy.
		m_sampler = nullptr;
		// m_view is owned by the registry's resolved view; no destroy.
		m_view = nullptr;
		if (m_atlasHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_atlasHandle);
			m_atlasHandle = {};
		}
		if (m_uploadPool != nullptr)
		{
			gpu::Factory::DestroyCommandPool(m_device, m_uploadPool);
			m_uploadPool = nullptr;
		}
		m_bindlessMgr = nullptr;
		m_device = nullptr;
	}

	// -- Queries ---------------------------------------------------------------

	bool FontAtlas::IsValid() const
	{
		return m_atlasHandle.IsValid();
	}

	uint32_t FontAtlas::GetBindlessSlot() const
	{
		return m_bindlessSlot;
	}

	const GlyphInfo& FontAtlas::GetGlyph(char cp) const
	{
		const int idx = static_cast<unsigned char>(cp) - kFirstChar;
		if (idx < 0 || idx >= kGlyphCount)
		{
			return m_fallbackGlyph;
		}
		return m_glyphs[idx];
	}
} // namespace aether
