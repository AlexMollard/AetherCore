#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include "vulkan/volk.hpp"
#include "vulkan/UniqueImage.hpp"
#include "gpu/GpuEnums.hpp"

namespace aether
{
	class BindlessManager;
	struct FrameTarget;

	// Per-frame allocation and execution statistics.
	struct FrameStats
	{
		std::uint32_t passCount = 0;
		std::uint32_t barrierCount = 0;
		std::uint32_t transientAllocated = 0;  // new GPU allocs this frame
		std::uint32_t transientCacheHit = 0;   // pulled from cache
		std::uint32_t transientCacheMiss = 0;  // had to allocate fresh
		std::uint32_t pendingDestructions = 0; // destroyed this BeginFrame
		std::size_t cacheSize = 0;             // total cached images
	};

	// Holds all Vulkan-internal state for RenderGraph.
	// RenderGraph.hpp sees only an opaque forward declaration; the
	// implementation in RenderGraph.cpp accesses members through this class.
	struct RenderGraphStorage
	{
		static constexpr std::size_t kMaxFramesInFlight = 3;
		static constexpr std::uint32_t kCacheMaxStaleFrames = 10;
		void Initialize(VkDevice device, VmaAllocator allocator);
		void Shutdown();
		void BeginFrame(std::uint32_t frameIndex);

		// ── External images ────────────────────────────────────────────────
		uint32_t RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect);
		[[nodiscard]] VkImage GetExternalImage(uint32_t idx) const;
		[[nodiscard]] VkImageView GetExternalView(uint32_t idx) const;
		[[nodiscard]] VkImageAspectFlags GetExternalAspect(uint32_t idx) const;

		[[nodiscard]] std::size_t GetExternalImageCount() const
		{
			return m_externalImages.size();
		}

		// ── Transient image slots ──────────────────────────────────────────
		uint32_t AddTransientSlot(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, gpu::Extent2D extent);
		void EnsureTransientImages(const FrameTarget& target);

		[[nodiscard]] VkImage ResolveTransientImage(uint32_t idx) const;
		[[nodiscard]] VkImageView ResolveTransientView(uint32_t idx) const;
		[[nodiscard]] VkImageAspectFlags ResolveTransientAspect(uint32_t idx) const;
		[[nodiscard]] gpu::Extent2D GetTransientAllocatedExtent(uint32_t idx) const;
		[[nodiscard]] bool IsTransientSlotValid(uint32_t idx) const;

		[[nodiscard]] std::size_t GetTransientCount() const
		{
			return m_transientImages.size();
		}

		// ── Bindless ───────────────────────────────────────────────────────
		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, BindlessManager& bindlessManager, VkDevice device, VkImageLayout descriptorLayout);
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(uint32_t transientIdx) const;

		// ── Release / cache ────────────────────────────────────────────────
		void ReleaseTransient(uint32_t idx, std::uint32_t currentFrame);

		// ── Image layout oracle (debug) ────────────────────────────────────
#ifndef NDEBUG
		void SetTrackedLayout(VkImage image, VkImageLayout layout)
		{
			m_trackedLayouts[image] = layout;
		}

		[[nodiscard]] VkImageLayout GetTrackedLayout(VkImage image) const
		{
			const auto it = m_trackedLayouts.find(image);
			return it != m_trackedLayouts.end() ? it->second : VK_IMAGE_LAYOUT_UNDEFINED;
		}

		void EraseTrackedLayout(VkImage image)
		{
			m_trackedLayouts.erase(image);
		}
#endif

		// ── Frame statistics ────────────────────────────────────────────
		[[nodiscard]] const FrameStats& GetLastFrameStats() const
		{
			return m_lastFrameStats;
		}

		[[nodiscard]] FrameStats& GetLastFrameStats()
		{
			return m_lastFrameStats;
		}

		// ── Scratch (reused across Execute calls) ──────────────────────────
		[[nodiscard]] std::vector<VkRenderingAttachmentInfo>& GetScratchColorInfos()
		{
			return m_scratchColorInfos;
		}

		[[nodiscard]] std::vector<VkImageMemoryBarrier2>& GetScratchBarriers()
		{
			return m_scratchBarriers;
		}

	private:
		// ── Internal types ─────────────────────────────────────────────────
		struct ExternalImageEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		struct TransientImageEntry
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImageUsageFlags usage = 0;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			gpu::Extent2D extent{};
			bool bindlessRequested = false;
			VkImageLayout bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			UniqueImage image;
			gpu::Extent2D allocatedExtent{};
			std::uint32_t aliasedEntryIndex = 0xFFFFFFFFu;
		};

		struct ImageCacheKey
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImageUsageFlags usage = 0;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t mipLevels = 1;
			VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

			bool operator==(const ImageCacheKey& other) const noexcept
			{
				return format == other.format && usage == other.usage && aspect == other.aspect && width == other.width && height == other.height && mipLevels == other.mipLevels && samples == other.samples;
			}
		};

		struct ImageCacheKeyHash
		{
			std::size_t operator()(const ImageCacheKey& k) const noexcept
			{
				std::size_t h = std::hash<uint32_t>{}(static_cast<uint32_t>(k.format));
				h ^= std::hash<uint32_t>{}(k.usage) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.aspect) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.mipLevels) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.samples)) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};

		struct CachedImage
		{
			UniqueImage image;
			std::uint32_t lastUsedFrame = 0;
		};

		struct PendingDestruction
		{
			std::uint32_t entryIndex = 0xFFFFFFFFu;
			UniqueImage image;
		};

		// ── Cache helpers ──────────────────────────────────────────────────
		void MoveToCache(TransientImageEntry& entry);
		UniqueImage TryPullFromCache(const ImageCacheKey& key);
		void EvictStaleCacheEntries();
		[[nodiscard]] ImageCacheKey MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent) const;

		// ── Member state ───────────────────────────────────────────────────
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;

		std::vector<ExternalImageEntry> m_externalImages;
		std::vector<TransientImageEntry> m_transientImages;
		std::vector<std::uint32_t> m_freeTransientSlots;
		std::unordered_map<ImageCacheKey, std::vector<CachedImage>, ImageCacheKeyHash> m_imageCache;

		std::vector<PendingDestruction> m_pendingDestructions[kMaxFramesInFlight];
		std::uint32_t m_currentFrame = 0;

		// Scratch buffers reused across Execute calls within a single frame.
		std::vector<VkRenderingAttachmentInfo> m_scratchColorInfos;
		std::vector<VkImageMemoryBarrier2> m_scratchBarriers;

		// Per-frame allocation statistics (populated during Execute).
		FrameStats m_lastFrameStats;

#ifndef NDEBUG
		// Debug-only layout oracle: tracks last-known layout for every image
		// known to the render graph. Seeded with UNDEFINED on allocation;
		// checked before each barrier in Execute() to catch layout mismatches.
		struct VkImageHash
		{
			std::size_t operator()(VkImage img) const noexcept
			{
				return std::hash<uint64_t>{}(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(img)));
			}
		};

		std::unordered_map<VkImage, VkImageLayout, VkImageHash> m_trackedLayouts;
#endif
	};
} // namespace aether
