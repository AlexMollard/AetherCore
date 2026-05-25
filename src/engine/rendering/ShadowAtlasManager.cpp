#include "rendering/ShadowAtlasManager.hpp"

#include "gpu/BindlessManager.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void ShadowAtlasManager::Initialize(const VulkanContext& ctx, BindlessManager& bindless)
	{
		m_bindless = &bindless;
		const VkDevice device = ctx.GetDevice().device;
		const VmaAllocator allocator = ctx.GetAllocator();

		AE_EXPECT_OR_THROW(img, UniqueImage::Create(device, allocator,
		        {
		                .extent = { kAtlasWidth, kAtlasHeight },
		                .format = kAtlasFormat,
		                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		                .debugName = "ShadowAtlas",
		        }));
		m_atlas = std::move(img);

		AE_EXPECT_OR_THROW_VOID(m_atlas.EnsureBindlessSampled(bindless, device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		m_bindlessSlot = m_atlas.GetBindlessSampledSlot();
	}

	void ShadowAtlasManager::Shutdown()
	{
		if (m_bindlessSlot != 0xFFFFFFFFu && m_bindless != nullptr)
		{
			m_atlas.ReleaseBindlessSampled();
		}
		m_atlas.Reset();
		m_bindless = nullptr;
		m_bindlessSlot = 0xFFFFFFFFu;
		m_shelves.clear();
	}

	void ShadowAtlasManager::Reset()
	{
		m_shelves.clear();
	}

	ShadowAtlasManager::Region ShadowAtlasManager::Allocate(const std::uint32_t width, const std::uint32_t height)
	{
		if (width == 0 || height == 0 || width > kAtlasWidth || height > kAtlasHeight)
		{
			return {};
		}

		// Try to fit into an existing shelf.
		for (Shelf& shelf: m_shelves)
		{
			if (shelf.height >= height && shelf.cursorX + width <= kAtlasWidth)
			{
				Region r{ shelf.cursorX, shelf.y, width, height };
				shelf.cursorX += width;
				return r;
			}
		}

		// Check if we can start a new shelf.
		const std::uint32_t nextY = m_shelves.empty() ? 0 : (m_shelves.back().y + m_shelves.back().height);
		if (nextY + height > kAtlasHeight)
		{
			return {};
		}

		m_shelves.push_back(Shelf{ nextY, height, width });
		return Region{ 0, nextY, width, height };
	}
} // namespace aether
