#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include "vulkan/volk.hpp"
#include "vulkan/UniqueImage.hpp"
#include "vulkan/UniqueBuffer.hpp"
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
		std::size_t aliasedImageCount = 0;     // transient images from heap
		std::size_t aliasedBufferCount = 0;    // transient buffers from heap
		VkDeviceSize heapCapacity = 0;         // total transient heap size
		VkDeviceSize heapUsed = 0;             // bytes used in transient heap
	};

	// Holds all Vulkan-internal state for RenderGraph.
	// RenderGraph.hpp sees only an opaque forward declaration; the
	// implementation in RenderGraph.cpp accesses members through this class.
	struct RenderGraphStorage
	{
		static constexpr std::size_t kMaxFramesInFlight = 3;
		static constexpr std::uint32_t kCacheMaxStaleFrames = 10;
		static constexpr VkDeviceSize kTransientHeapCapacity = 256ull * 1024 * 1024; // 256 MB
		static constexpr VkDeviceSize kTransientHeapAlignment = 65536u;
		void Initialize(VkDevice device, VmaAllocator allocator);
		// Engine-side overload: opaque gpu::Device / gpu::Allocator.
		void Initialize(gpu::Device device, gpu::Allocator allocator);
		void Shutdown();
		void BeginFrame(std::uint32_t frameIndex);

		// -- Async compute multi-queue support -------------------------------
		// Call once after Initialize() when a dedicated compute queue is
		// available. Creates per-frame compute command pools/buffers/fences
		// and a cross-queue timeline semaphore.
		void EnableAsyncCompute(VkQueue computeQueue, std::uint32_t computeQueueFamily);
		// Engine-side overload: opaque gpu::Queue.
		void EnableAsyncCompute(gpu::Queue computeQueue, std::uint32_t computeQueueFamily);

		// Begin recording async compute passes. Must be called before any
		// compute-queue pass executes. Signals the per-frame fence from the
		// previous frame, resets it, resets the command pool, and begins the
		// command buffer with ONE_TIME_SUBMIT_BIT.
		void BeginComputeCommandBuffer(std::uint32_t frameIndex);

		// Returns the VkCommandBuffer for async compute passes (valid after
		// BeginComputeCommandBuffer).
		[[nodiscard]] VkCommandBuffer GetComputeCommandBuffer(std::uint32_t frameIndex) const;

		// Ends the async compute command buffer.
		void EndComputeCommandBuffer(std::uint32_t frameIndex);

		// Submits the compute command buffer to the dedicated compute queue.
		// Signals m_crossQueueTimeline at the next timeline value (m_crossQueueTimelineValue + 1).
		// The caller passes the returned semaphore + value to the graphics
		// queue submission as a wait.
		void SubmitComputeQueue(std::uint32_t frameIndex);

		// Timeline semaphore handle (reinterpret_cast to VkSemaphore) and
		// current signal value. Valid after SubmitComputeQueue() when
		// async compute is enabled.
		[[nodiscard]] std::uint64_t GetCrossQueueTimelineSemaphore() const
		{
			return reinterpret_cast<std::uint64_t>(m_crossQueueTimeline);
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

		// -- External images ------------------------------------------------
		uint32_t RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect);
		// Engine-side overload: opaque gpu::Image / gpu::ImageView / gpu::ImageAspect.
		uint32_t RegisterExternalImage(gpu::Image image, gpu::ImageView view, gpu::ImageAspect aspect);
		[[nodiscard]] VkImage GetExternalImage(uint32_t idx) const;
		[[nodiscard]] VkImageView GetExternalView(uint32_t idx) const;
		[[nodiscard]] VkImageAspectFlags GetExternalAspect(uint32_t idx) const;
		void ReleaseExternal(uint32_t idx);
		void ClearExternalImages();

		[[nodiscard]] std::size_t GetExternalImageCount() const
		{
			return m_externalImages.size();
		}

		// -- External buffers ------------------------------------------------
		uint32_t RegisterExternalBuffer(VkBuffer buffer);
		// Engine-side overload: opaque gpu::Buffer.
		uint32_t RegisterExternalBuffer(gpu::Buffer buffer);
		void UpdateExternalBuffer(uint32_t idx, VkBuffer buffer);
		// Engine-side overload: opaque gpu::Buffer.
		void UpdateExternalBuffer(uint32_t idx, gpu::Buffer buffer);
		[[nodiscard]] VkBuffer GetExternalBuffer(uint32_t idx) const;
		void ReleaseExternalBuffer(uint32_t idx);

		[[nodiscard]] std::size_t GetExternalBufferCount() const
		{
			return m_externalBuffers.size();
		}

		// -- Transient image slots ------------------------------------------
		uint32_t AddTransientSlot(VkFormat format, gpu::ImageUsage usage, gpu::ImageAspect aspect, gpu::Extent2D extent);
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

		// -- Bindless -------------------------------------------------------
		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, BindlessManager& bindlessManager, VkDevice device, VkImageLayout descriptorLayout);
		// Engine-side overload: opaque gpu::Device / gpu::ImageLayout.
		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, BindlessManager& bindlessManager, gpu::Device device, gpu::ImageLayout descriptorLayout);
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(uint32_t transientIdx) const;

		// -- Transient buffer slots ------------------------------------------
		uint32_t AddTransientBufferSlot(VkDeviceSize size, VkBufferUsageFlags usage);
		void EnsureTransientBuffers();

		[[nodiscard]] VkBuffer ResolveTransientBuffer(uint32_t idx) const;
		[[nodiscard]] bool IsTransientBufferSlotValid(uint32_t idx) const;

		[[nodiscard]] std::size_t GetTransientBufferCount() const
		{
			return m_transientBuffers.size();
		}

		void ReleaseTransientBuffer(uint32_t idx);

		// -- Two-pass transient heap preparation (called after Compile). ----
		void PrepareTransientAllocations(const FrameTarget& target);

		// -- Release / cache ------------------------------------------------
		void ReleaseTransient(uint32_t idx, std::uint32_t currentFrame);

		// -- Image layout oracle (debug) ------------------------------------
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

		// -- Frame statistics --------------------------------------------
		[[nodiscard]] const FrameStats& GetLastFrameStats() const
		{
			return m_lastFrameStats;
		}

		[[nodiscard]] FrameStats& GetLastFrameStats()
		{
			return m_lastFrameStats;
		}

		// -- Split barrier events ------------------------------------------------
		std::uint32_t AllocateEvent();
		[[nodiscard]] VkEvent GetEvent(std::uint32_t eventIndex) const;
		void ReleaseEvent(std::uint32_t eventIndex);
		void ResetEvents();

		// Emit Vulkan commands for split barriers (vkCmdSetEvent2 / vkCmdWaitEvents2).
		void CmdSetEvent2(VkCommandBuffer cmd, VkEvent event, const VkImageMemoryBarrier2* barriers, uint32_t count);
		void CmdWaitEvents2(VkCommandBuffer cmd, VkEvent event, const VkImageMemoryBarrier2* barriers, uint32_t count);

		// Emit buffer memory barriers via vkCmdPipelineBarrier2.
		void CmdBufferBarriers(VkCommandBuffer cmd, const VkBufferMemoryBarrier2* barriers, uint32_t count);

		// -- Scratch (reused across Execute calls) --------------------------
		[[nodiscard]] std::vector<VkRenderingAttachmentInfo>& GetScratchColorInfos()
		{
			return m_scratchColorInfos;
		}

		[[nodiscard]] std::vector<VkImageMemoryBarrier2>& GetScratchBarriers()
		{
			return m_scratchBarriers;
		}

		[[nodiscard]] std::vector<VkImageMemoryBarrier2>& GetScratchSignalBarriers()
		{
			return m_scratchSignalBarriers;
		}

		[[nodiscard]] std::vector<VkBufferMemoryBarrier2>& GetScratchBufferBarriers()
		{
			return m_scratchBufferBarriers;
		}

	private:
		// -- Internal types -------------------------------------------------
		struct ExternalImageEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		using VirtualAllocationHandle = VmaVirtualAllocation;

		struct TransientImageEntry
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::Extent2D extent{};
			bool bindlessRequested = false;
			bool fromHeap = false; // true if allocated from transient heap
			VkImageLayout bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			UniqueImage image;
			gpu::Extent2D allocatedExtent{};
			std::uint32_t aliasedEntryIndex = 0xFFFFFFFFu;
			// Filled by PrepareTransientAllocations (two-pass).
			VkDeviceSize memReqSize = 0;
			VkDeviceSize memReqAlignment = 0;
			VkDeviceSize heapOffset = VK_WHOLE_SIZE; // offset into transient heap
			VirtualAllocationHandle m_virtualAlloc = nullptr;
		};

		struct TransientBufferEntry
		{
			VkDeviceSize size = 0;
			VkBufferUsageFlags usage = 0;
			bool fromHeap = false;
			UniqueBuffer buffer;
			// Filled by PrepareTransientAllocations (two-pass).
			VkDeviceSize memReqSize = 0;
			VkDeviceSize memReqAlignment = 0;
			VkDeviceSize heapOffset = VK_WHOLE_SIZE; // offset into transient heap
			VirtualAllocationHandle m_virtualAlloc = nullptr;
		};

		struct ImageCacheKey
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
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
			UniqueImage image;
			std::uint32_t lastUsedFrame = 0;
		};

		struct PendingDestruction
		{
			std::uint32_t entryIndex = 0xFFFFFFFFu;
			UniqueImage image;
		};

		// -- Cache helpers --------------------------------------------------
		void MoveToCache(TransientImageEntry& entry);
		UniqueImage TryPullFromCache(const ImageCacheKey& key);
		void EvictStaleCacheEntries();
		[[nodiscard]] ImageCacheKey MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent) const;

		static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		void AllocateTransientHeap(VkDeviceSize requiredSize);

		// -- Member state ---------------------------------------------------
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;

		std::vector<ExternalImageEntry> m_externalImages;
		std::vector<std::uint32_t> m_freeExternalSlots;
		std::vector<VkBuffer> m_externalBuffers;
		std::vector<std::uint32_t> m_freeExternalBufferSlots;
		std::vector<TransientImageEntry> m_transientImages;
		std::vector<std::uint32_t> m_freeTransientSlots;
		std::vector<TransientBufferEntry> m_transientBuffers;
		std::vector<std::uint32_t> m_freeTransientBufferSlots;
		std::unordered_map<ImageCacheKey, std::vector<CachedImage>, ImageCacheKeyHash> m_imageCache;

		std::vector<PendingDestruction> m_pendingDestructions[kMaxFramesInFlight];
		std::uint32_t m_currentFrame = 0;

		// Scratch buffers reused across Execute calls within a single frame.
		std::vector<VkRenderingAttachmentInfo> m_scratchColorInfos;
		std::vector<VkImageMemoryBarrier2> m_scratchBarriers;
		std::vector<VkImageMemoryBarrier2> m_scratchSignalBarriers;
		std::vector<VkBufferMemoryBarrier2> m_scratchBufferBarriers;

		// Event pool for split barriers.
		std::vector<VkEvent> m_events;
		std::vector<std::uint32_t> m_freeEventSlots;

		// Transient heap for VRAM-aliased images and buffers (VmaVirtualBlock).
		VmaAllocation m_transientHeapAllocation = VK_NULL_HANDLE;
		VmaVirtualBlock m_virtualBlock = VK_NULL_HANDLE;
		VkDeviceSize m_transientHeapCapacity = 0;

		// Per-frame allocation statistics (populated during Execute).
		FrameStats m_lastFrameStats;

		// -- Async compute multi-queue state ---------------------------------
		struct ComputeFrameResources
		{
			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
		};

		std::array<ComputeFrameResources, kMaxFramesInFlight> m_computeFrames{};
		VkQueue m_computeQueue = VK_NULL_HANDLE;
		std::uint32_t m_computeQueueFamily = 0;
		VkSemaphore m_crossQueueTimeline = VK_NULL_HANDLE;
		std::uint64_t m_crossQueueTimelineValue = 0;
		bool m_asyncComputeEnabled = false;

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
