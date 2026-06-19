#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>

#include "gpu/ResourceRegistry.hpp"
#include "gpu/Semaphore.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/volk.hpp"
#include "gpu/GpuEnums.hpp"

namespace aether
{
	class BindlessManager;
	class VulkanContext;
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
		// Engine-side overload: opaque gpu::Device / gpu::Allocator.
		void Initialize(gpu::Device device, gpu::Allocator allocator);

		// Set the VulkanContext for device-loss routing. When a compute fence
		// returns VK_ERROR_DEVICE_LOST, BeginComputeCommandBuffer calls
		// WaitIdle() on this context instead of throwing directly, so the
		// DiagnosticEngine captures the fault before the exception propagates.
		void SetVulkanContext(VulkanContext* ctx)
		{
			m_vulkanContext = ctx;
		}

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

		// Returns the engine-side handle for the async compute command buffer
		// (valid after BeginComputeCommandBuffer).
		[[nodiscard]] gpu::CommandBuffer GetComputeCommandBuffer(std::uint32_t frameIndex) const;

		// Ends the async compute command buffer.
		void EndComputeCommandBuffer(std::uint32_t frameIndex);

		// Submits the compute command buffer to the dedicated compute queue.
		// Signals m_crossQueueTimeline at the next timeline value (m_crossQueueTimelineValue + 1).
		// The caller passes the returned semaphore + value to the graphics
		// queue submission as a wait.
		void SubmitComputeQueue(std::uint32_t frameIndex);

		// Timeline semaphore handle (engine-side typed `gpu::TimelineSemaphoreHandle`)
		// and current signal value. Valid after SubmitComputeQueue() when
		// async compute is enabled. The handle is owned by the storage
		// (it is destroyed in `Shutdown`).
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

		// -- External images ------------------------------------------------
		uint32_t RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect);
		// Engine-side overload: opaque gpu::Image / gpu::ImageView / gpu::ImageAspect.
		uint32_t RegisterExternalImage(gpu::Image image, gpu::ImageView view, gpu::ImageAspect aspect);
		// Engine-side resolution (P5(d)). Returns the opaque gpu::Image /
		// gpu::ImageView / gpu::ImageAspect; the storage's typed result is
		// the same numeric value as the underlying Vk* (the audit's
		// borrowed-vs-owned rule: Image / ImageView are borrowed opaque
		// handles, not typed handles).
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

		// -- External buffers ------------------------------------------------
		uint32_t RegisterExternalBuffer(VkBuffer buffer);
		// Engine-side overload: opaque gpu::Buffer.
		uint32_t RegisterExternalBuffer(gpu::Buffer buffer);
		void UpdateExternalBuffer(uint32_t idx, VkBuffer buffer);
		// Engine-side overload: opaque gpu::Buffer.
		void UpdateExternalBuffer(uint32_t idx, gpu::Buffer buffer);
		[[nodiscard]] gpu::Buffer GetExternalBuffer(uint32_t idx) const;
		[[nodiscard]] VkBuffer GetExternalBufferVk(uint32_t idx) const;
		void ReleaseExternalBuffer(uint32_t idx);

		[[nodiscard]] std::size_t GetExternalBufferCount() const
		{
			return m_externalBuffers.size();
		}

		// -- Transient image slots ------------------------------------------
		uint32_t AddTransientSlot(gpu::Format format, gpu::ImageUsage usage, gpu::ImageAspect aspect, gpu::Extent2D extent);
		void EnsureTransientImages(const FrameTarget& target);

		// Engine-side resolution (P5(d)).
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

		// -- Bindless -------------------------------------------------------
		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, VkImageLayout descriptorLayout);
		std::uint32_t EnsureBindlessSampled(uint32_t transientIdx, gpu::ImageLayout descriptorLayout);
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(uint32_t transientIdx) const;

		// -- Transient buffer slots ------------------------------------------
		uint32_t AddTransientBufferSlot(VkDeviceSize size, VkBufferUsageFlags2 usage);
		void EnsureTransientBuffers();

		[[nodiscard]] gpu::Buffer ResolveTransientBuffer(uint32_t idx) const;
		[[nodiscard]] VkBuffer ResolveTransientBufferVk(uint32_t idx) const;
		[[nodiscard]] bool IsTransientBufferSlotValid(uint32_t idx) const;

		[[nodiscard]] std::size_t GetTransientBufferCount() const
		{
			return m_transientBuffers.size();
		}

		void ReleaseTransientBuffer(uint32_t idx);

		// -- Two-pass transient heap preparation (called after Compile). ----
		void PrepareTransientAllocations(const FrameTarget& target);

		// -- Release / cache ------------------------------------------------
		void ReleaseTransient(uint32_t idx);

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
		[[nodiscard]] gpu::Event GetEvent(std::uint32_t eventIndex) const;
		[[nodiscard]] VkEvent GetEventVk(std::uint32_t eventIndex) const;
		void ReleaseEvent(std::uint32_t eventIndex);
		void ResetEvents();

		// Emit Vulkan commands for split barriers (vkCmdSetEvent2 / vkCmdWaitEvents2).
		// Engine-side: takes a gpu::ImageMemoryBarrier span. The barrier's
		// opaque gpu::Image field is the pre-resolved VkImage (callers in
		// RenderGraph.cpp populate it via their resolveImage lambda before
		// calling these methods).
		static void CmdSetEvent2(gpu::CommandBuffer cmd, gpu::Event event, std::span<const gpu::ImageMemoryBarrier> barriers);
		static void CmdWaitEvents2(gpu::CommandBuffer cmd, gpu::Event event, std::span<const gpu::ImageMemoryBarrier> barriers);

		// Emit buffer memory barriers via vkCmdPipelineBarrier2. Engine-side:
		// takes a gpu::BufferMemoryBarrier span. The barrier's opaque
		// gpu::Buffer field is the pre-resolved VkBuffer.
		static void CmdBufferBarriers(gpu::CommandBuffer cmd, std::span<const gpu::BufferMemoryBarrier> barriers);

		// Emit image memory barriers via vkCmdPipelineBarrier2. Engine-side:
		// takes a gpu::ImageMemoryBarrier span. The barrier's opaque
		// gpu::Image field is the pre-resolved VkImage.
		static void CmdImageBarriers(gpu::CommandBuffer cmd, std::span<const gpu::ImageMemoryBarrier> barriers);

		// -- Scratch (reused across Execute calls) --------------------------
		// Engine-side scratch arrays (P5(d)). The barrier emitter methods
		// above accept a std::span, so callers (RenderGraph.cpp) build local
		// std::vector<gpu::ImageMemoryBarrier> and pass a span. These scratch
		// accessors are kept for callers that prefer storage-owned scratch.
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
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::Extent2D extent;
			bool bindlessRequested = false;
			bool fromHeap = false; // true if allocated from transient heap
			VkImageLayout bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			gpu::TextureHandle image;
			gpu::Extent2D allocatedExtent;
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
			VkBufferUsageFlags2 usage = 0;
			bool fromHeap = false;
			gpu::BufferHandle buffer;
			// Filled by PrepareTransientAllocations (two-pass).
			VkDeviceSize memReqSize = 0;
			VkDeviceSize memReqAlignment = 0;
			VkDeviceSize heapOffset = VK_WHOLE_SIZE; // offset into transient heap
			VirtualAllocationHandle m_virtualAlloc = nullptr;
		};

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

		// -- Cache helpers --------------------------------------------------
		void MoveToCache(TransientImageEntry& entry);
		gpu::TextureHandle TryPullFromCache(const ImageCacheKey& key);
		void EvictStaleCacheEntries();
		[[nodiscard]] static ImageCacheKey MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent);

		static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		void AllocateTransientHeap(VkDeviceSize requiredSize);

		// -- Member state ---------------------------------------------------
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

		// Scratch buffers reused across Execute calls within a single frame.
		std::vector<gpu::RenderingAttachmentInfo> m_scratchColorInfos;
		std::vector<gpu::ImageMemoryBarrier> m_scratchBarriers;
		std::vector<gpu::ImageMemoryBarrier> m_scratchSignalBarriers;
		std::vector<gpu::BufferMemoryBarrier> m_scratchBufferBarriers;

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
		gpu::TimelineSemaphoreHandle m_crossQueueTimeline = nullptr;
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
