#include "vulkan/RenderGraphStorage.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <numeric>
#include <queue>

#include "gpu/BindlessManager.hpp"
#include "gpu/FrameTarget.hpp"
#include "gpu/GpuEnums.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	void RenderGraphStorage::Initialize(VkDevice device, VmaAllocator allocator)
	{
		m_device = device;
		m_allocator = allocator;
	}

	void RenderGraphStorage::Shutdown()
	{
		for (auto& entry: m_transientImages)
		{
			entry.image.Reset();
			entry.aliasedEntryIndex = 0xFFFFFFFFu;
			entry.allocatedExtent = {};
		}
		m_transientImages.clear();
		m_externalImages.clear();

		for (auto& [key, cachedList]: m_imageCache)
		{
			for (auto& ci: cachedList)
			{
				ci.image.Reset();
			}
		}
		m_imageCache.clear();
		m_freeTransientSlots.clear();

		for (std::size_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			m_pendingDestructions[i].clear();
		}

		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

	void RenderGraphStorage::BeginFrame(std::uint32_t frameIndex)
	{
		m_currentFrame = frameIndex % kMaxFramesInFlight;

		m_lastFrameStats = FrameStats{};

		auto& toDestroy = m_pendingDestructions[m_currentFrame];
		m_lastFrameStats.pendingDestructions = static_cast<std::uint32_t>(toDestroy.size());
		for (auto& pending: toDestroy)
		{
			pending.image.Reset();
		}
		toDestroy.clear();
	}

	// -- External images ------------------------------------------------------

	uint32_t RenderGraphStorage::RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect)
	{
		const uint32_t idx = static_cast<uint32_t>(m_externalImages.size());
		m_externalImages.push_back({image, view, aspect});
#ifndef NDEBUG
		SetTrackedLayout(image, VK_IMAGE_LAYOUT_UNDEFINED);
#endif
		return idx;
	}

	VkImage RenderGraphStorage::GetExternalImage(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].image : VK_NULL_HANDLE;
	}

	VkImageView RenderGraphStorage::GetExternalView(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].view : VK_NULL_HANDLE;
	}

	VkImageAspectFlags RenderGraphStorage::GetExternalAspect(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].aspect : VK_IMAGE_ASPECT_COLOR_BIT;
	}

	// -- Transient images -----------------------------------------------------

	uint32_t RenderGraphStorage::AddTransientSlot(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, gpu::Extent2D extent)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: AddTransientSlot called before Initialize().");
		}

		if (!m_freeTransientSlots.empty())
		{
			const uint32_t idx = m_freeTransientSlots.back();
			m_freeTransientSlots.pop_back();
			auto& entry = m_transientImages[idx];
			entry = {};
			entry.format = format;
			entry.usage = usage;
			entry.aspect = aspect;
			entry.extent = extent;
			return idx;
		}

		TransientImageEntry entry{};
		entry.format = format;
		entry.usage = usage;
		entry.aspect = aspect;
		entry.extent = extent;
		m_transientImages.push_back(std::move(entry));
		return static_cast<uint32_t>(m_transientImages.size() - 1);
	}

	VkImage RenderGraphStorage::ResolveTransientImage(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return VK_NULL_HANDLE;
		}
		const auto& entry = m_transientImages[idx];
		if (entry.image)
		{
			return entry.image.Get();
		}
		if (entry.aliasedEntryIndex < m_transientImages.size())
		{
			AE_ASSERT(m_transientImages[entry.aliasedEntryIndex].image, "Alias target must own image - check aliasing logic");
			return m_transientImages[entry.aliasedEntryIndex].image.Get();
		}
		return VK_NULL_HANDLE;
	}

	VkImageView RenderGraphStorage::ResolveTransientView(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return VK_NULL_HANDLE;
		}
		const auto& entry = m_transientImages[idx];
		if (entry.image)
		{
			return entry.image.GetDefaultView();
		}
		if (entry.aliasedEntryIndex < m_transientImages.size())
		{
			AE_ASSERT(m_transientImages[entry.aliasedEntryIndex].image, "Alias target must own image - check aliasing logic");
			return m_transientImages[entry.aliasedEntryIndex].image.GetDefaultView();
		}
		return VK_NULL_HANDLE;
	}

	VkImageAspectFlags RenderGraphStorage::ResolveTransientAspect(uint32_t idx) const
	{
		if (idx < m_transientImages.size())
		{
			return m_transientImages[idx].aspect;
		}
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}

	gpu::Extent2D RenderGraphStorage::GetTransientAllocatedExtent(uint32_t idx) const
	{
		if (idx < m_transientImages.size())
		{
			return m_transientImages[idx].allocatedExtent;
		}
		return {};
	}

	bool RenderGraphStorage::IsTransientSlotValid(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return false;
		}
		const auto& entry = m_transientImages[idx];
		return entry.image || entry.aliasedEntryIndex < m_transientImages.size();
	}

	// -- Bindless -------------------------------------------------------------

	std::uint32_t RenderGraphStorage::EnsureBindlessSampled(uint32_t transientIdx, BindlessManager& bindlessManager, VkDevice device, VkImageLayout descriptorLayout)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			return 0xFFFFFFFFu;
		}
		if (transientIdx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		auto& entry = m_transientImages[transientIdx];
		entry.bindlessRequested = true;
		entry.bindlessLayout = descriptorLayout;
		entry.aliasedEntryIndex = 0xFFFFFFFFu;

		if (!entry.image)
		{
			if (entry.format == VK_FORMAT_UNDEFINED || entry.usage == 0 || entry.extent.width == 0 || entry.extent.height == 0)
			{
				return 0xFFFFFFFFu;
			}

			AE_EXPECT_OR_THROW(newImage,
			        UniqueImage::Create(m_device,
			                m_allocator,
			                {
			                        .extent = entry.extent,
			                        .format = gpu::FromVk(entry.format),
			                        .usage = entry.usage,
			                }));
			entry.image = std::move(newImage);
			entry.allocatedExtent = entry.extent;
		}

		AE_EXPECT_OR_THROW_VOID(entry.image.EnsureBindlessSampled(bindlessManager, device, entry.aspect, descriptorLayout));
		return entry.image.GetBindlessSampledSlot();
	}

	std::uint32_t RenderGraphStorage::GetBindlessSampledSlot(uint32_t transientIdx) const
	{
		if (transientIdx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		const auto& entry = m_transientImages[transientIdx];
		if (!entry.image.HasBindlessSampled())
		{
			return 0xFFFFFFFFu;
		}
		return entry.image.GetBindlessSampledSlot();
	}

	// -- Release / cache ------------------------------------------------------

	void RenderGraphStorage::ReleaseTransient(uint32_t idx, std::uint32_t currentFrame)
	{
		if (idx >= m_transientImages.size())
		{
			return;
		}

		auto& entry = m_transientImages[idx];
		if (entry.bindlessRequested)
		{
#ifndef NDEBUG
			if (entry.image)
			{
				EraseTrackedLayout(entry.image.Get());
			}
#endif
			PendingDestruction pending{};
			pending.entryIndex = idx;
			pending.image = std::move(entry.image);
			m_pendingDestructions[currentFrame % kMaxFramesInFlight].push_back(std::move(pending));
		}
		else
		{
			MoveToCache(entry);
		}
		entry.bindlessRequested = false;
		entry.bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		entry.aliasedEntryIndex = 0xFFFFFFFFu;
		entry.format = VK_FORMAT_UNDEFINED;
		entry.usage = 0;
		entry.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		entry.extent = {};
		m_freeTransientSlots.push_back(idx);
	}

	// -- Cache helpers --------------------------------------------------------

	RenderGraphStorage::ImageCacheKey RenderGraphStorage::MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent) const
	{
		return ImageCacheKey{
		        .format = entry.format,
		        .usage = entry.usage,
		        .aspect = entry.aspect,
		        .width = extent.width,
		        .height = extent.height,
		        .mipLevels = 1,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		};
	}

	void RenderGraphStorage::MoveToCache(TransientImageEntry& entry)
	{
		if (!entry.image)
		{
			return;
		}
#ifndef NDEBUG
		EraseTrackedLayout(entry.image.Get());
#endif
		const ImageCacheKey key = MakeCacheKey(entry, entry.allocatedExtent);
		m_imageCache[key].push_back(CachedImage{
		        .image = std::move(entry.image),
		        .lastUsedFrame = m_currentFrame,
		});
		entry.allocatedExtent = {};
	}

	UniqueImage RenderGraphStorage::TryPullFromCache(const ImageCacheKey& key)
	{
		auto it = m_imageCache.find(key);
		if (it == m_imageCache.end() || it->second.empty())
		{
			return {};
		}
		UniqueImage img = std::move(it->second.back().image);
		it->second.pop_back();
		if (it->second.empty())
		{
			m_imageCache.erase(it);
		}
		return img;
	}

	void RenderGraphStorage::EvictStaleCacheEntries()
	{
		for (auto it = m_imageCache.begin(); it != m_imageCache.end();)
		{
			auto& list = it->second;
			std::erase_if(list,
			        [&](const CachedImage& ci)
			        {
				        const std::uint32_t age = (m_currentFrame >= ci.lastUsedFrame) ? (m_currentFrame - ci.lastUsedFrame) : (kMaxFramesInFlight + m_currentFrame - ci.lastUsedFrame);
				        return age > kCacheMaxStaleFrames;
			        });
			if (list.empty())
			{
				it = m_imageCache.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// -- EnsureTransientImages ------------------------------------------------

	void RenderGraphStorage::EnsureTransientImages(const FrameTarget& target)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: transient images require Initialize(device, allocator).");
			return;
		}

		for (std::uint32_t idx = 0; idx < m_transientImages.size(); ++idx)
		{
			auto& entry = m_transientImages[idx];
			if (entry.image)
			{
				continue;
			}
			if (entry.format == VK_FORMAT_UNDEFINED || entry.usage == 0)
			{
				continue;
			}
			if (entry.extent.width == 0 || entry.extent.height == 0)
			{
				entry.extent = target.extent;
			}
			if (entry.extent.width == 0 || entry.extent.height == 0)
			{
				continue;
			}

			const ImageCacheKey key = MakeCacheKey(entry, entry.extent);
			UniqueImage cached = TryPullFromCache(key);
			if (cached)
			{
				entry.image = std::move(cached);
				entry.allocatedExtent = entry.extent;
				m_lastFrameStats.transientCacheHit++;
#ifndef NDEBUG
				SetTrackedLayout(entry.image.Get(), VK_IMAGE_LAYOUT_UNDEFINED);
#endif
			}
			else
			{
				AE_EXPECT_OR_THROW(newImage,
				        UniqueImage::Create(m_device,
				                m_allocator,
				                {
				                        .extent = entry.extent,
				                        .format = gpu::FromVk(entry.format),
				                        .usage = entry.usage,
				                }));
				entry.image = std::move(newImage);
				entry.allocatedExtent = entry.extent;
				m_lastFrameStats.transientAllocated++;
				m_lastFrameStats.transientCacheMiss++;
				const std::string entryName = entry.bindlessRequested ? std::format("RenderGraph.Transient.Bindless[{}]", idx) : std::format("RenderGraph.Transient[{}]", idx);
				entry.image.SetName(m_device, entryName.c_str());
#ifndef NDEBUG
				SetTrackedLayout(entry.image.Get(), VK_IMAGE_LAYOUT_UNDEFINED);
#endif
			}
		}

		EvictStaleCacheEntries();

		// Compute total cache occupancy.
		m_lastFrameStats.cacheSize = 0;
		for (const auto& [key, entries]: m_imageCache)
		{
			m_lastFrameStats.cacheSize += entries.size();
		}
	}
} // namespace aether
