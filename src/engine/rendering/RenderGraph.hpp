#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "rendering/CommandRecorder.hpp"
#include "utils/GpuProfiler.hpp"
#include "vulkan/UniqueImage.hpp"

namespace aether
{
	class BindlessManager;

	// Opaque handle to a render-graph-managed image resource.
	// Acquired from RenderGraph::GetSwapchainColor/Depth or future
	// CreateTransient*.
	struct RGImage
	{
		static constexpr uint32_t kInvalid = ~0u;
		uint32_t id = kInvalid;

		[[nodiscard]] bool IsValid() const
		{
			return id != kInvalid;
		}
	};

	// Helpers for constructing VkClearValue without nested brace issues on MSVC.
	[[nodiscard]] inline VkClearValue ClearColorValue(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f) noexcept
	{
		VkClearValue v{};
		v.color.float32[0] = r;
		v.color.float32[1] = g;
		v.color.float32[2] = b;
		v.color.float32[3] = a;
		return v;
	}

	[[nodiscard]] inline VkClearValue ClearDepthValue(float depth = 1.0f, uint32_t stencil = 0u) noexcept
	{
		VkClearValue v{};
		v.depthStencil.depth = depth;
		v.depthStencil.stencil = stencil;
		return v;
	}

	// Data made available inside pass execute callbacks.
	struct PassContext
	{
		CommandRecorder& recorder;
		VkExtent2D extent;
		std::uint64_t frameConstantsAddr = 0;
		std::uint32_t frameIndex = 0;
	};

	// Per-frame swapchain handles supplied to RenderGraph::Execute by AetherCore.
	struct FrameTarget
	{
		VkImage colorImage = VK_NULL_HANDLE;
		VkImageView colorView = VK_NULL_HANDLE;
		VkImage depthImage = VK_NULL_HANDLE;
		VkImageView depthView = VK_NULL_HANDLE;
		VkFormat colorFormat = VK_FORMAT_UNDEFINED;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D extent{};
	};

	// Frame graph with pass/resource declarations and automatic image barriers.
	class RenderGraph
	{
	public:
		struct TransientImageDesc
		{
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImageUsageFlags usage = 0;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			VkExtent2D extent{}; // {0,0} = match FrameTarget extent at Execute()
		};

		void Initialize(VkDevice device, VmaAllocator allocator);
		void Shutdown();

		// Begin a new frame - must be called before Execute() to process
		// deferred destructions from frames the GPU has finished.
		void BeginFrame(std::uint32_t frameIndex);

		// Optional Tracy GPU context for GPU-zone instrumentation of render passes.
		void SetTracyVkCtx(TracyVkCtx ctx)
		{
			m_tracyVkCtx = ctx;
		}

		// Fluent pass builder; use immediately, do not store.
		class PassBuilder
		{
		public:
			PassBuilder(PassBuilder&&) = default;
			PassBuilder(const PassBuilder&) = AE_DELETE_MSG("PassBuilder is move-only - use std::move");
			PassBuilder& operator=(PassBuilder&&) = AE_DELETE_MSG("PassBuilder is move-only - use std::move");

			// Declare a color attachment write.
			PassBuilder& WriteColor(RGImage image, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE, VkClearValue clearValue = {});

			// Declare a depth/stencil attachment write.
			PassBuilder& WriteDepth(RGImage image, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, VkClearValue clearValue = {});

			// Declare a sampled texture read.
			PassBuilder& ReadTexture(RGImage image);

			// Declare a sampled texture read in compute.
			PassBuilder& ReadTextureCompute(RGImage image);

			// Declare a storage-image read in compute.
			PassBuilder& ReadStorageImage(RGImage image);

			// Declare a storage-image write in compute.
			PassBuilder& WriteStorageImage(RGImage image);

			// Set graphics callback for this pass.
			PassBuilder& Execute(std::function<void(PassContext&)> fn);

			// Set compute callback for this pass.
			PassBuilder& ExecuteCompute(std::function<void(PassContext&)> fn);

			// Override pass extent (for render-to-texture and non-swapchain targets).
			PassBuilder& SetExtent(VkExtent2D extent);

		private:
			friend class RenderGraph;
			PassBuilder(RenderGraph& graph, std::size_t passIndex);

			RenderGraph& m_graph;
			std::size_t m_passIndex;
		};

		[[nodiscard]] RGImage GetSwapchainColor() const
		{
			return RGImage{kSwapchainColorId};
		}

		[[nodiscard]] RGImage GetSwapchainDepth() const
		{
			return RGImage{kSwapchainDepthId};
		}

		// Register an externally-owned image and return an RGImage handle.
		[[nodiscard]] RGImage RegisterImage(VkImage image, VkImageView view, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

		// Create a render-graph-owned transient image.
		[[nodiscard]] RGImage CreateTransientImage(const TransientImageDesc& desc);

		// Convenience helpers for transient color/depth attachments.
		[[nodiscard]] RGImage CreateTransientColor(VkFormat format, VkExtent2D extent = {}, VkImageUsageFlags extraUsage = 0);
		[[nodiscard]] RGImage CreateTransientDepth(VkFormat format, VkExtent2D extent = {}, VkImageUsageFlags extraUsage = 0);

		// Ensure a transient image is registered for bindless sampled access.
		// Returns 0xFFFFFFFF when image is invalid/non-transient/not allocatable.
		[[nodiscard]] std::uint32_t EnsureBindlessSampled(RGImage image, BindlessManager& bindlessManager, VkDevice device, VkImageLayout descriptorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// Returns bindless slot for a transient image if already registered.
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(RGImage image) const;

		// Release a registered image handle from the graph.
		// For transients this also destroys owned GPU memory.
		void ReleaseImage(RGImage image);

		// Register a graphics pass.
		[[nodiscard]] PassBuilder AddPass(std::string name);

		// Register a compute pass.
		[[nodiscard]] PassBuilder AddComputePass(std::string name);

		void RemovePass(const std::string& name);
		[[nodiscard]] bool HasPass(std::string_view name) const;
		void Clear();

		[[nodiscard]] bool IsEmpty() const
		{
			return m_passes.empty();
		}

		struct PassInfo
		{
			std::string name;
			bool isGraphics = false;
			bool isCompute = false;
			float lastCpuTimeMs = 0.f;
		};

		[[nodiscard]] std::vector<PassInfo> GetPasses() const;

		// Execute the compiled frame graph for the current frame.
		void Execute(CommandRecorder& recorder, const FrameTarget& target, std::uint64_t frameConstantsAddr, std::uint32_t frameIndex);

	private:
		static constexpr uint32_t kSwapchainColorId = 0u;
		static constexpr uint32_t kSwapchainDepthId = 1u;
		static constexpr uint32_t kFirstExternalId = 2u;
		static constexpr uint32_t kFirstTransientId = 0x40000000u;

		// Entry for each image registered via RegisterImage().
		struct ExternalImageEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		struct TransientImageEntry
		{
			TransientImageDesc desc{};
			bool bindlessRequested = false;
			VkImageLayout bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			UniqueImage image;
			std::uint32_t aliasedEntryIndex = 0xFFFFFFFFu; // index of entry we alias (when image is null)
			VkExtent2D allocatedExtent{};
		};

		struct AttachmentRef
		{
			RGImage image{};
			VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkClearValue clearValue{};
		};

		enum class ImageAccessType
		{
			SampledRead,
			StorageRead,
			StorageWrite,
		};

		struct ImageAccessRef
		{
			RGImage image{};
			ImageAccessType type = ImageAccessType::SampledRead;
		};

		enum class PassKind
		{
			Graphics,
			Compute,
		};

		struct PassRecord
		{
			std::string name;
			PassKind kind = PassKind::Graphics;
			std::vector<AttachmentRef> colorWrites;
			std::optional<AttachmentRef> depthWrite;
			std::vector<ImageAccessRef> imageAccesses;
			std::function<void(PassContext&)> execute;
			std::optional<VkExtent2D> extentOverride; // if set, overrides target.extent
			float lastCpuTimeMs = 0.f;
		};

		struct CompiledBarrier
		{
			uint32_t resourceId = 0;
			VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
			VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		struct CompiledPass
		{
			std::size_t passIndex = 0;
			std::vector<CompiledBarrier> preBarriers;
		};

		struct ResourceState
		{
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkPipelineStageFlags2 writeStage = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 writeAccess = VK_ACCESS_2_NONE;
			bool isCrossFrame = false;
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

		void MoveToCache(TransientImageEntry& entry);
		UniqueImage TryPullFromCache(const ImageCacheKey& key);
		void EvictStaleCacheEntries();
		[[nodiscard]] ImageCacheKey MakeCacheKey(const TransientImageDesc& desc, VkExtent2D extent) const;

		void Compile();
		void EnsureTransientImages(const FrameTarget& target);

		[[nodiscard]] VkImage ResolveImage(uint32_t resourceId, const FrameTarget& target) const;
		[[nodiscard]] VkImageView ResolveView(uint32_t resourceId, const FrameTarget& target) const;
		[[nodiscard]] VkImageAspectFlags ResolveAspect(uint32_t resourceId) const;
		[[nodiscard]] bool IsTransientId(uint32_t resourceId) const;

		std::vector<PassRecord> m_passes;
		std::vector<CompiledPass> m_compiled;
		std::vector<ExternalImageEntry> m_externalImages;   // indexed by (id - kFirstExternalId)
		std::vector<TransientImageEntry> m_transientImages; // indexed by (id - kFirstTransientId)
		std::unordered_map<uint32_t, ResourceState> m_lastImageStates;
		std::unordered_map<ImageCacheKey, std::vector<CachedImage>, ImageCacheKeyHash> m_imageCache;
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		TracyVkCtx m_tracyVkCtx = nullptr;

		bool m_compileDirty = true;

		std::vector<VkRenderingAttachmentInfo> m_scratchColorInfos;
		std::vector<VkImageMemoryBarrier2> m_scratchBarriers;

		// Deferred destruction: images are queued for destruction and only
		// actually destroyed when the GPU has finished the frame they were
		// used in. Indexed by frame index % kMaxFramesInFlight.
		static constexpr std::size_t kMaxFramesInFlight = 3;
		static constexpr std::uint32_t kCacheMaxStaleFrames = 10;

		struct PendingDestruction
		{
			std::uint32_t entryIndex = 0xFFFFFFFFu;
			UniqueImage image;
		};

		std::vector<PendingDestruction> m_pendingDestructions[kMaxFramesInFlight];
		std::uint32_t m_currentFrame = 0;

	public:
		static RenderGraph* s_current;
	};

	[[nodiscard]] inline RenderGraph* GetCurrentRenderGraph()
	{
		return RenderGraph::s_current;
	}
} // namespace aether
