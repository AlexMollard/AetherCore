#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include <span>

#include "gpu/ResourceRegistry.hpp"
#include "gpu/Semaphore.hpp"
#include "rendering/TransientHeapPacker.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/volk.hpp"
#include "gpu/GpuEnums.hpp"

namespace aether
{
	class BindlessManager;
	class VulkanContext;
	struct FrameTarget;

	// Every field falls into one of three classes, and the class decides when it is
	// written. Mixing them is what made these counters unreadable: a per-frame counter
	// that is only touched on the rare frame that allocates reads zero forever.
	struct FrameStats
	{
		// -- Per frame: cleared by BeginFrame, rewritten while the frame is built.
		std::uint32_t passCount = 0;
		std::uint32_t barrierCount = 0;

		// -- Levels: recomputed from live state every frame, so a poll is always current.
		std::uint32_t transientImageCount = 0;
		std::uint32_t transientBufferCount = 0;
		std::uint32_t pendingDestructions = 0;
		std::size_t cacheSize = 0;
		// Backed by the shared heap.
		std::size_t pooledImageCount = 0;
		std::size_t pooledBufferCount = 0;
		// Of those, the ones whose heap range is shared with another resource.
		std::size_t aliasedImageCount = 0;
		std::size_t aliasedBufferCount = 0;
		VkDeviceSize heapCapacity = 0;
		VkDeviceSize heapUsed = 0;
		// What the transients would cost with a dedicated allocation each, versus what
		// they actually cost once the heap plan overlaps disjoint lifetimes.
		VkDeviceSize transientLogicalBytes = 0;
		VkDeviceSize transientPhysicalBytes = 0;

		// -- Cumulative since Initialize(): allocation is an event, not a level. These
		// only move when the graph is rebuilt or a target resizes, which is exactly the
		// signal worth watching, and a per-frame reading of it is always zero.
		std::uint64_t transientAllocated = 0;
		std::uint64_t transientCacheHit = 0;
		std::uint64_t transientCacheMiss = 0;
	};

	struct RenderGraphStorage
	{
		static constexpr std::size_t kMaxFramesInFlight = 3;
		static constexpr std::uint32_t kCacheMaxStaleFrames = 10;
		static constexpr VkDeviceSize kTransientHeapCapacity = 256ull * 1024 * 1024;
		static constexpr VkDeviceSize kTransientHeapAlignment = 65536u;

		RenderGraphStorage() = default;

		~RenderGraphStorage()
		{
			Shutdown();
		}

		RenderGraphStorage(const RenderGraphStorage&) = delete;
		RenderGraphStorage& operator=(const RenderGraphStorage&) = delete;
		RenderGraphStorage(RenderGraphStorage&&) = delete;
		RenderGraphStorage& operator=(RenderGraphStorage&&) = delete;

		void Initialize(VkDevice device, VmaAllocator allocator);
		void Initialize(gpu::Device device, gpu::Allocator allocator);

		void SetVulkanContext(VulkanContext* ctx)
		{
			m_vulkanContext = ctx;
		}

		void Shutdown();
		void BeginFrame(std::uint32_t frameIndex);

		void EnableAsyncCompute(VkQueue computeQueue, std::uint32_t computeQueueFamily);
		void EnableAsyncCompute(gpu::Queue computeQueue, std::uint32_t computeQueueFamily);

		// Begin recording async compute passes. Must be called before any
		void BeginComputeCommandBuffer(std::uint32_t frameIndex);

		[[nodiscard]] gpu::CommandBuffer GetComputeCommandBuffer(std::uint32_t frameIndex) const;

		void EndComputeCommandBuffer(std::uint32_t frameIndex);

		void SubmitComputeQueue(std::uint32_t frameIndex);

		[[nodiscard]] gpu::TimelineSemaphoreHandle GetCrossQueueTimelineSemaphore() const
		{
			return m_crossQueueTimeline;
		}

		[[nodiscard]] std::uint64_t GetCrossQueueTimelineValue() const
		{
			return m_crossQueueTimelineValue;
		}

		[[nodiscard]] bool IsAsyncComputeEnabled() const
		{
			return m_asyncComputeEnabled;
		}

		void ShutdownComputeResources();

		uint32_t RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect);
		uint32_t RegisterExternalImage(gpu::Image image, gpu::ImageView view, gpu::ImageAspect aspect);
		[[nodiscard]] gpu::Image GetExternalImage(uint32_t idx) const;
		[[nodiscard]] gpu::ImageView GetExternalView(uint32_t idx) const;
		[[nodiscard]] gpu::ImageAspect GetExternalAspect(uint32_t idx) const;
		[[nodiscard]] VkImage GetExternalImageVk(uint32_t idx) const;
		[[nodiscard]] VkImageView GetExternalViewVk(uint32_t idx) const;
		[[nodiscard]] VkImageAspectFlags GetExternalAspectVk(uint32_t idx) const;
		void ReleaseExternal(uint32_t idx);
		void ClearExternalImages();

		[[nodiscard]] std::size_t GetExternalImageCount() const
		{
			return m_externalImages.size();
		}

		uint32_t RegisterExternalBuffer(VkBuffer buffer);
		uint32_t RegisterExternalBuffer(gpu::Buffer buffer);
		void UpdateExternalBuffer(uint32_t idx, VkBuffer buffer);
		void UpdateExternalBuffer(uint32_t idx, gpu::Buffer buffer);
		[[nodiscard]] gpu::Buffer GetExternalBuffer(uint32_t idx) const;
		[[nodiscard]] VkBuffer GetExternalBufferVk(uint32_t idx) const;
		void ReleaseExternalBuffer(uint32_t idx);
		void ClearExternalBuffers();

		[[nodiscard]] std::size_t GetExternalBufferCount() const
		{
			return m_externalBuffers.size();
		}

		uint32_t AddTransientSlot(gpu::Format format, gpu::ImageUsage usage, gpu::ImageAspect aspect, gpu::Extent2D extent);
		void EnsureTransientImages(const FrameTarget& target);

		[[nodiscard]] gpu::Image ResolveTransientImage(uint32_t idx) const;
		[[nodiscard]] gpu::ImageView ResolveTransientView(uint32_t idx) const;
		[[nodiscard]] gpu::ImageAspect ResolveTransientAspect(uint32_t idx) const;
		[[nodiscard]] VkImage ResolveTransientImageVk(uint32_t idx) const;
		[[nodiscard]] VkImageView ResolveTransientViewVk(uint32_t idx) const;
		[[nodiscard]] VkImageAspectFlags ResolveTransientAspectVk(uint32_t idx) const;
		[[nodiscard]] gpu::Extent2D GetTransientAllocatedExtent(uint32_t idx) const;
		[[nodiscard]] bool IsTransientSlotValid(uint32_t idx) const;

		[[nodiscard]] std::size_t GetTransientCount() const
		{
			return m_transientImages.size();
		}

		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, VkImageLayout descriptorLayout);
		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, gpu::ImageLayout descriptorLayout);
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(uint32_t transientIdx) const;

		uint32_t AddTransientBufferSlot(VkDeviceSize size, VkBufferUsageFlags2 usage);
		void EnsureTransientBuffers();

		[[nodiscard]] gpu::Buffer ResolveTransientBuffer(uint32_t idx) const;
		[[nodiscard]] gpu::BufferHandle ResolveTransientBufferHandle(uint32_t idx) const;
		[[nodiscard]] VkBuffer ResolveTransientBufferVk(uint32_t idx) const;
		[[nodiscard]] bool IsTransientBufferSlotValid(uint32_t idx) const;

		[[nodiscard]] std::size_t GetTransientBufferCount() const
		{
			return m_transientBuffers.size();
		}

		void ReleaseTransientBuffer(uint32_t idx);

		// Lays every transient the graph can back out of one pooled allocation, overlapping
		// the ones whose lifetimes do not. The lifetime spans are indexed by transient slot.
		void PrepareTransientAllocations(const FrameTarget& target, std::span<const TransientLifetime> imageLifetimes, std::span<const TransientLifetime> bufferLifetimes);

		// Recomputes every level-class FrameStats field from live state. Must run after the
		// transients for the frame have been materialised.
		void RefreshTransientStats();

		[[nodiscard]] bool IsTransientImageAliased(std::uint32_t idx) const;
		[[nodiscard]] bool IsTransientBufferAliased(std::uint32_t idx) const;

		void ReleaseTransient(uint32_t idx);
		void ReleaseAllTransients();

		// -- Image layout oracle (debug) ------------------------------------
#ifndef NDEBUG
		void SetTrackedLayout(gpu::Image image, gpu::ImageLayout layout)
		{
			m_trackedLayouts[static_cast<VkImage>(image)] = gpu::ToVk(layout);
		}

		[[nodiscard]] gpu::ImageLayout GetTrackedLayout(gpu::Image image) const
		{
			const auto it = m_trackedLayouts.find(static_cast<VkImage>(image));
			if (it == m_trackedLayouts.end())
			{
				return gpu::ImageLayout::Undefined;
			}
			return gpu::FromVk(it->second);
		}

		void EraseTrackedLayout(gpu::Image image)
		{
			m_trackedLayouts.erase(static_cast<VkImage>(image));
		}
#endif

		[[nodiscard]] const FrameStats& GetLastFrameStats() const
		{
			return m_lastFrameStats;
		}

		[[nodiscard]] FrameStats& GetLastFrameStats()
		{
			return m_lastFrameStats;
		}

		std::uint32_t AllocateEvent();
		[[nodiscard]] gpu::Event GetEvent(std::uint32_t eventIndex) const;
		[[nodiscard]] VkEvent GetEventVk(std::uint32_t eventIndex) const;
		void ReleaseEvent(std::uint32_t eventIndex);
		void ResetEvents();

		static void CmdSetEvent2(gpu::CommandBuffer cmd, gpu::Event event, std::span<const gpu::ImageMemoryBarrier> barriers);
		static void CmdWaitEvents2(gpu::CommandBuffer cmd, gpu::Event event, std::span<const gpu::ImageMemoryBarrier> barriers);

		static void CmdBufferBarriers(gpu::CommandBuffer cmd, std::span<const gpu::BufferMemoryBarrier> barriers);

		static void CmdImageBarriers(gpu::CommandBuffer cmd, std::span<const gpu::ImageMemoryBarrier> barriers);

		[[nodiscard]] std::vector<gpu::RenderingAttachmentInfo>& GetScratchColorInfos()
		{
			return m_scratchColorInfos;
		}

		[[nodiscard]] std::vector<gpu::ImageMemoryBarrier>& GetScratchBarriers()
		{
			return m_scratchBarriers;
		}

		[[nodiscard]] std::vector<gpu::ImageMemoryBarrier>& GetScratchSignalBarriers()
		{
			return m_scratchSignalBarriers;
		}

		[[nodiscard]] std::vector<gpu::BufferMemoryBarrier>& GetScratchBufferBarriers()
		{
			return m_scratchBufferBarriers;
		}

	private:
		struct ExternalImageEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		struct HeapRequestSource
		{
			std::uint32_t index = 0;
			bool isBuffer = false;
		};

		struct TransientImageEntry
		{
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::Extent2D extent;
			bool bindlessRequested = false;
			// Some compiled pass touches the slot. A slot nothing touches is not worth the
			// VRAM, so it is left unmaterialised until the graph starts using it again.
			bool live = false;
			bool fromHeap = false;
			// Shares its heap range with at least one other resource, so its contents do not
			// survive the frame and its first use has to discard.
			bool aliased = false;
			VkImageLayout bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			// Reserved when the slot is first asked for and held for as long as the graph
			// keeps the slot, so callers may cache the number. Every image that comes to back
			// this slot - pooled, aliased, recycled from the cache - is written into it.
			std::uint32_t reservedBindlessSlot = 0xFFFFFFFFu;
			// The image currently pointed at by reservedBindlessSlot. Re-binding is skipped
			// while this still matches, so a steady-state frame writes no descriptors.
			gpu::TextureHandle boundBindlessImage;
			gpu::TextureHandle image;
			gpu::Extent2D allocatedExtent;
			std::uint32_t aliasedEntryIndex = 0xFFFFFFFFu;
			VkDeviceSize memReqSize = 0;
			VkDeviceSize memReqAlignment = 0;
			std::uint32_t memReqTypeBits = 0;
			VkDeviceSize heapOffset = VK_WHOLE_SIZE;
		};

		struct TransientBufferEntry
		{
			VkDeviceSize size = 0;
			VkBufferUsageFlags2 usage = 0;
			bool live = false;
			bool fromHeap = false;
			bool aliased = false;
			gpu::BufferHandle buffer;
			VkDeviceSize memReqSize = 0;
			VkDeviceSize memReqAlignment = 0;
			std::uint32_t memReqTypeBits = 0;
			VkDeviceSize heapOffset = VK_WHOLE_SIZE;
		};

		// Points the entry's reserved bindless slot at whatever image currently backs it.
		// A no-op when the slot already names that image, so a steady-state frame writes
		// no descriptors at all.
		static void BindReservedSlot(TransientImageEntry& entry);

		struct ImageCacheKey
		{
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
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
				h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.usage)) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.aspect)) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.mipLevels) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.samples)) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};

		struct CachedImage
		{
			gpu::TextureHandle handle;
			std::uint32_t lastUsedFrame = 0;
		};

		void MoveToCache(TransientImageEntry& entry);
		gpu::TextureHandle TryPullFromCache(const ImageCacheKey& key);
		void EvictStaleCacheEntries();
		[[nodiscard]] static ImageCacheKey MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent);

		static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		void AllocateTransientHeap(VkDeviceSize requiredSize, VkDeviceSize alignment, std::uint32_t memoryTypeBits);
		void ReleaseTransientHeap();
		[[nodiscard]] static std::uint64_t HashHeapPlan(std::span<const TransientHeapRequest> requests, const TransientHeapPlan& plan);

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VulkanContext* m_vulkanContext = nullptr;

		std::vector<ExternalImageEntry> m_externalImages;
		std::vector<std::uint32_t> m_freeExternalSlots;
		std::vector<VkBuffer> m_externalBuffers;
		std::vector<std::uint32_t> m_freeExternalBufferSlots;
		std::vector<TransientImageEntry> m_transientImages;
		std::vector<std::uint32_t> m_freeTransientSlots;
		std::vector<TransientBufferEntry> m_transientBuffers;
		std::vector<std::uint32_t> m_freeTransientBufferSlots;
		std::unordered_map<ImageCacheKey, std::vector<CachedImage>, ImageCacheKeyHash> m_imageCache;

		std::uint32_t m_currentFrame = 0;

		std::vector<gpu::RenderingAttachmentInfo> m_scratchColorInfos;
		std::vector<gpu::ImageMemoryBarrier> m_scratchBarriers;
		std::vector<gpu::ImageMemoryBarrier> m_scratchSignalBarriers;
		std::vector<gpu::BufferMemoryBarrier> m_scratchBufferBarriers;
		std::vector<TransientHeapRequest> m_scratchHeapRequests;
		std::vector<HeapRequestSource> m_scratchHeapSources;

		std::vector<VkEvent> m_events;
		std::vector<std::uint32_t> m_freeEventSlots;

		VmaAllocation m_transientHeapAllocation = VK_NULL_HANDLE;
		VkDeviceSize m_transientHeapCapacity = 0;
		VkDeviceSize m_transientHeapAlignment = kTransientHeapAlignment;
		// Identifies the plan the live residents were laid out by. A plan is only re-applied
		// when the graph changes shape, because applying one destroys every resident.
		std::uint64_t m_heapPlanSignature = 0;
		std::uint64_t m_heapPlanStandaloneSize = 0;
		bool m_reportedMemoryTypeConflict = false;
		bool m_reportedHeapAllocFailure = false;

		FrameStats m_lastFrameStats;

		struct ComputeFrameResources
		{
			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
		};

		std::array<ComputeFrameResources, kMaxFramesInFlight> m_computeFrames{};
		VkQueue m_computeQueue = VK_NULL_HANDLE;
		std::uint32_t m_computeQueueFamily = 0;
		gpu::TimelineSemaphoreHandle m_crossQueueTimeline = nullptr;
		std::uint64_t m_crossQueueTimelineValue = 0;
		bool m_asyncComputeEnabled = false;

#ifndef NDEBUG
		// Debug-only layout oracle: tracks last-known layout for every image
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
