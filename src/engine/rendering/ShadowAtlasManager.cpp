#include "rendering/ShadowAtlasManager.hpp"

#include "gpu/BindlessManager.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void ShadowAtlasManager::Initialize(BindlessManager& bindless)
	{
		AE_PROFILE_ZONE();
		m_bindless = &bindless;

		const gpu::TextureDesc desc{
		        .format = kAtlasFormat,
		        .extent = {kAtlasWidth, kAtlasHeight},
		        .usage = gpu::ImageUsage::TransferSrc | gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::Storage | gpu::ImageUsage::ColorAttachment,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "ShadowAtlas",
		};
		m_atlasHandle = gpu::ResourceRegistry::CreateTexture(desc);
		if (!m_atlasHandle.IsValid())
		{
			Throw(AetherError::Engine("ShadowAtlasManager: CreateTexture failed"));
		}

		m_atlasImage = gpu::ResourceRegistry::ResolveTextureImage(m_atlasHandle);
		m_atlasView = gpu::ResourceRegistry::ResolveTexture(m_atlasHandle).view;

		// Bindless registration: acquire a slot, then update the descriptor
		// with the resolved image view + a linear sampler.
		const auto slotResult = bindless.AllocateSampledImageSlot();
		if (!slotResult)
		{
			Throw(AetherError::Engine("ShadowAtlasManager: AllocateSampledImageSlot failed"));
		}
		m_bindlessSlot = *slotResult;
		const auto updateResult = bindless.WriteSampledImage(m_bindlessSlot, gpu::ResourceRegistry::GetViewCreateInfo(m_atlasHandle), gpu::ImageLayout::ShaderReadOnly);
		if (!updateResult)
		{
			Throw(AetherError::Engine("ShadowAtlasManager: WriteSampledImage failed"));
		}
	}

	void ShadowAtlasManager::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_atlasHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_atlasHandle);
		}
		m_atlasHandle = {};
		m_atlasImage = nullptr;
		m_atlasView = nullptr;
		m_bindless = nullptr;
		m_bindlessSlot = 0xFFFFFFFFu;
		m_shelves.clear();
	}

	void ShadowAtlasManager::Reset()
	{
		AE_PROFILE_ZONE();
		m_shelves.clear();
		m_usedBounds = {};
	}

	ShadowAtlasManager::Region ShadowAtlasManager::Allocate(const std::uint32_t width, const std::uint32_t height)
	{
		AE_PROFILE_ZONE();
		if (width == 0 || height == 0 || width > kAtlasWidth || height > kAtlasHeight)
		{
			return {};
		}

		// Try to fit into an existing shelf.
		for (Shelf& shelf: m_shelves)
		{
			if (shelf.height >= height && shelf.cursorX + width <= kAtlasWidth)
			{
				Region r{.x = shelf.cursorX, .y = shelf.y, .width = width, .height = height};
				shelf.cursorX += width;

				// Update used bounds
				if (m_usedBounds.width == 0)
				{
					m_usedBounds = r;
				}
				else
				{
					const std::uint32_t minX = std::min(m_usedBounds.x, r.x);
					const std::uint32_t minY = std::min(m_usedBounds.y, r.y);
					const std::uint32_t maxX = std::max(m_usedBounds.x + m_usedBounds.width, r.x + r.width);
					const std::uint32_t maxY = std::max(m_usedBounds.y + m_usedBounds.height, r.y + r.height);
					m_usedBounds = {.x = minX, .y = minY, .width = maxX - minX, .height = maxY - minY};
				}

				return r;
			}
		}

		// Check if we can start a new shelf.
		const std::uint32_t nextY = m_shelves.empty() ? 0 : (m_shelves.back().y + m_shelves.back().height);
		if (nextY + height > kAtlasHeight)
		{
			return {};
		}

		m_shelves.push_back(Shelf{.y = nextY, .height = height, .cursorX = width});
		Region r{.x = 0, .y = nextY, .width = width, .height = height};

		// Update used bounds
		if (m_usedBounds.width == 0)
		{
			m_usedBounds = r;
		}
		else
		{
			const std::uint32_t minX = std::min(m_usedBounds.x, r.x);
			const std::uint32_t minY = std::min(m_usedBounds.y, r.y);
			const std::uint32_t maxX = std::max(m_usedBounds.x + m_usedBounds.width, r.x + r.width);
			const std::uint32_t maxY = std::max(m_usedBounds.y + m_usedBounds.height, r.y + r.height);
			m_usedBounds = {.x = minX, .y = minY, .width = maxX - minX, .height = maxY - minY};
		}

		return r;
	}
} // namespace aether
