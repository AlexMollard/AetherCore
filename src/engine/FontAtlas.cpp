#include "FontAtlas.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "BindlessManager.hpp"
#include "FileSystem.hpp"
#include "Logger.hpp"

namespace aether
{
	namespace
	{
		void TransitionImage(
			VkCommandBuffer       cmd,
			VkImage               image,
			VkImageLayout         oldLayout,
			VkImageLayout         newLayout,
			VkPipelineStageFlags2 srcStage,
			VkAccessFlags2        srcAccess,
			VkPipelineStageFlags2 dstStage,
			VkAccessFlags2        dstAccess)
		{
			const VkImageMemoryBarrier2 barrier{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = srcStage,
				.srcAccessMask = srcAccess,
				.dstStageMask = dstStage,
				.dstAccessMask = dstAccess,
				.oldLayout = oldLayout,
				.newLayout = newLayout,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = image,
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			};
			const VkDependencyInfo dep{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrier,
			};
			vkCmdPipelineBarrier2(cmd, &dep);
		}

		VkCommandBuffer BeginOneShot(VkDevice device, VkCommandPool pool)
		{
			const VkCommandBufferAllocateInfo ai{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			vkAllocateCommandBuffers(device, &ai, &cmd);

			const VkCommandBufferBeginInfo bi{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			vkBeginCommandBuffer(cmd, &bi);
			return cmd;
		}

		void EndAndSubmit(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
		{
			vkEndCommandBuffer(cmd);
			const VkSubmitInfo si{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &cmd,
			};
			vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
			vkQueueWaitIdle(queue);
			vkFreeCommandBuffers(device, pool, 1, &cmd);
		}
	}

	// ── Move semantics ────────────────────────────────────────────────────────

	FontAtlas::FontAtlas(FontAtlas&& o) noexcept
		: m_allocator(std::exchange(o.m_allocator, nullptr))
		, m_device(std::exchange(o.m_device, VK_NULL_HANDLE))
		, m_image(std::exchange(o.m_image, VK_NULL_HANDLE))
		, m_view(std::exchange(o.m_view, VK_NULL_HANDLE))
		, m_sampler(std::exchange(o.m_sampler, VK_NULL_HANDLE))
		, m_allocation(std::exchange(o.m_allocation, VK_NULL_HANDLE))
		, m_bindlessSlot(std::exchange(o.m_bindlessSlot, 0xFFFFFFFFu))
		, m_atlasWidth(std::exchange(o.m_atlasWidth, 0u))
		, m_atlasHeight(std::exchange(o.m_atlasHeight, 0u))
		, m_glyphSize(std::exchange(o.m_glyphSize, 0))
		, m_glyphs(o.m_glyphs)
	{}

	FontAtlas& FontAtlas::operator=(FontAtlas&& o) noexcept
	{
		if (this != &o)
		{
			Destroy();
			m_allocator = std::exchange(o.m_allocator, nullptr);
			m_device = std::exchange(o.m_device, VK_NULL_HANDLE);
			m_image = std::exchange(o.m_image, VK_NULL_HANDLE);
			m_view = std::exchange(o.m_view, VK_NULL_HANDLE);
			m_sampler = std::exchange(o.m_sampler, VK_NULL_HANDLE);
			m_allocation = std::exchange(o.m_allocation, VK_NULL_HANDLE);
			m_bindlessSlot = std::exchange(o.m_bindlessSlot, 0xFFFFFFFFu);
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

	// ── Build ─────────────────────────────────────────────────────────────────

	void FontAtlas::Build(
		std::string_view fontVfsPath,
		int              atlasGlyphSize,
		VkDevice         device,
		VmaAllocator     allocator,
		VkQueue          uploadQueue,
		uint32_t         uploadQueueFamily,
		BindlessManager& bindless)
	{
		// ── 1. Initialise FreeType ────────────────────────────────────────────
		FT_Library ft{};
		if (FT_Init_FreeType(&ft) != 0)
			throw std::runtime_error("FontAtlas: FT_Init_FreeType failed.");

		const std::vector<std::byte> fontData = io::FileSystem::ReadFile(fontVfsPath);
		if (fontData.empty())
		{
			FT_Done_FreeType(ft);
			throw std::runtime_error(
				std::string("FontAtlas: font not found at '") + std::string(fontVfsPath) + "'.");
		}

		FT_Face face{};
		if (FT_New_Memory_Face(
			ft,
			reinterpret_cast<const FT_Byte*>(fontData.data()),
			static_cast<FT_Long>(fontData.size()),
			0,
			&face) != 0)
		{
			FT_Done_FreeType(ft);
			throw std::runtime_error(
				std::string("FontAtlas: FT_New_Memory_Face failed for '") + std::string(fontVfsPath) + "'.");
		}

		FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(atlasGlyphSize));

		FT_UInt spread = static_cast<FT_UInt>(kSdfSpread);
		FT_Property_Set(ft, "sdf", "spread", &spread);

		// ── 2. Render all glyphs into a CPU-side atlas ────────────────────────
		const int cellSize = atlasGlyphSize + 2 * kSdfSpread;

		const int cols = static_cast<int>(std::sqrt(static_cast<double>(kGlyphCount))) + 1;
		const int rows = (kGlyphCount + cols - 1) / cols;

		auto nextPow2 = [](int v) {
			int p = 1;
			while (p < v) p <<= 1;
			return p;
			};

		const uint32_t atlasW = static_cast<uint32_t>(nextPow2(cols * cellSize));
		const uint32_t atlasH = static_cast<uint32_t>(nextPow2(rows * cellSize));

		std::vector<std::uint8_t> atlasPixels(atlasW * atlasH, 0u);

		const float fAtlasW = static_cast<float>(atlasW);
		const float fAtlasH = static_cast<float>(atlasH);

		for (int i = 0; i < kGlyphCount; ++i)
		{
			const char cp = static_cast<char>(kFirstChar + i);
			const FT_ULong charcode = static_cast<FT_ULong>(static_cast<unsigned char>(cp));
			const FT_UInt  glyphIdx = FT_Get_Char_Index(face, charcode);

			GlyphInfo& info = m_glyphs[i];

			if (glyphIdx == 0) { info = {}; continue; }

			FT_Error loadErr = FT_Load_Glyph(face, glyphIdx, FT_LOAD_DEFAULT);
			if (loadErr != 0) { info = {}; continue; }

			FT_Error renderErr = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF);
			if (renderErr != 0)
				renderErr = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
			if (renderErr != 0) { info = {}; continue; }

			const FT_Bitmap& bm = face->glyph->bitmap;

			const int col = i % cols;
			const int row = i / cols;
			const int origX = col * cellSize;
			const int origY = row * cellSize;

			for (unsigned int by = 0; by < bm.rows; ++by)
			{
				const int dstY = origY + static_cast<int>(by);
				if (dstY < 0 || dstY >= static_cast<int>(atlasH)) continue;

				for (unsigned int bx = 0; bx < bm.width; ++bx)
				{
					const int dstX = origX + static_cast<int>(bx);
					if (dstX < 0 || dstX >= static_cast<int>(atlasW)) continue;

					atlasPixels[static_cast<std::size_t>(dstY) * atlasW + dstX] =
						bm.buffer[by * static_cast<unsigned>(std::abs(bm.pitch)) + bx];
				}
			}

			const float emScale = 1.0f / static_cast<float>(atlasGlyphSize);
			info.uvRect = glm::vec4(
				static_cast<float>(origX) / fAtlasW,
				static_cast<float>(origY) / fAtlasH,
				static_cast<float>(origX + static_cast<int>(bm.width)) / fAtlasW,
				static_cast<float>(origY + static_cast<int>(bm.rows)) / fAtlasH);
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
		m_device = device;
		m_allocator = allocator;

		// ── 3. Upload atlas to GPU ────────────────────────────────────────────
		const VkDeviceSize imageBytes = atlasW * atlasH;

		const VkBufferCreateInfo stagingBufInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = imageBytes,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		const VmaAllocationCreateInfo stagingAllocInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
					 VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO,
		};
		VkBuffer      stagingBuf{};
		VmaAllocation stagingAlloc{};
		VmaAllocationInfo stagingInfo{};
		vmaCreateBuffer(allocator, &stagingBufInfo, &stagingAllocInfo,
			&stagingBuf, &stagingAlloc, &stagingInfo);

		std::memcpy(stagingInfo.pMappedData, atlasPixels.data(), imageBytes);

		const VkImageCreateInfo imgInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_R8_UNORM,
			.extent = { atlasW, atlasH, 1 },
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		const VmaAllocationCreateInfo imgAllocInfo{ .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
		vmaCreateImage(allocator, &imgInfo, &imgAllocInfo, &m_image, &m_allocation, nullptr);

		const VkImageViewCreateInfo viewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_R8_UNORM,
			.components = {
				VK_COMPONENT_SWIZZLE_R,
				VK_COMPONENT_SWIZZLE_ZERO,
				VK_COMPONENT_SWIZZLE_ZERO,
				VK_COMPONENT_SWIZZLE_ONE,
			},
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		vkCreateImageView(device, &viewInfo, nullptr, &m_view);

		const VkSamplerCreateInfo samplerInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.maxLod = VK_LOD_CLAMP_NONE,
		};
		vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler);

		const VkCommandPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			.queueFamilyIndex = uploadQueueFamily,
		};
		VkCommandPool uploadPool{};
		vkCreateCommandPool(device, &poolInfo, nullptr, &uploadPool);

		VkCommandBuffer cmd = BeginOneShot(device, uploadPool);

		TransitionImage(cmd, m_image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

		const VkBufferImageCopy copy{
			.bufferOffset = 0,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageOffset = { 0, 0, 0 },
			.imageExtent = { atlasW, atlasH, 1 },
		};
		vkCmdCopyBufferToImage(cmd, stagingBuf, m_image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

		TransitionImage(cmd, m_image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

		EndAndSubmit(device, uploadPool, uploadQueue, cmd);
		vkDestroyCommandPool(device, uploadPool, nullptr);
		vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);

		// ── 4. Register in the bindless descriptor set ────────────────────────
		m_bindlessSlot = bindless.AllocateSampledImageSlot();
		bindless.UpdateSampledImage(m_bindlessSlot, m_view, m_sampler);

		INFO(LogCategory::Asset,
			"FontAtlas built: {} glyphs, atlas {}x{}, bindless slot {}.",
			kGlyphCount, atlasW, atlasH, m_bindlessSlot);
	}

	// ── Destroy ───────────────────────────────────────────────────────────────

	void FontAtlas::Destroy()
	{
		if (m_device == VK_NULL_HANDLE)
			return;

		if (m_sampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(m_device, m_sampler, nullptr);
			m_sampler = VK_NULL_HANDLE;
		}
		if (m_view != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_device, m_view, nullptr);
			m_view = VK_NULL_HANDLE;
		}
		if (m_image != VK_NULL_HANDLE && m_allocator != nullptr)
		{
			vmaDestroyImage(m_allocator, m_image, m_allocation);
			m_image = VK_NULL_HANDLE;
			m_allocation = VK_NULL_HANDLE;
		}
		m_bindlessSlot = 0xFFFFFFFFu;
		m_device = VK_NULL_HANDLE;
		m_allocator = nullptr;
	}

	// ── Queries ───────────────────────────────────────────────────────────────

	bool FontAtlas::IsValid() const
	{
		return m_image != VK_NULL_HANDLE;
	}

	uint32_t FontAtlas::GetBindlessSlot() const
	{
		return m_bindlessSlot;
	}

	const GlyphInfo& FontAtlas::GetGlyph(char cp) const
	{
		const int idx = static_cast<unsigned char>(cp) - kFirstChar;
		if (idx < 0 || idx >= kGlyphCount)
			return m_fallbackGlyph;
		return m_glyphs[idx];
	}
}
