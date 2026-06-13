#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/FrameTarget.hpp"
#include "gpu/GpuEnums.hpp"
#include "utils/GpuProfiler.hpp"

namespace aether
{
	class BindlessManager;

	// Forward declarations - live in vulkan/RenderGraphStorage.hpp
	struct RenderGraphStorage;
	struct FrameStats;

	// Which hardware queue a pass executes on.
	enum class QueueClass : uint8_t
	{
		Graphics,     // main graphics queue (all rendering, inline compute)
		AsyncCompute, // dedicated async compute queue (culling, lighting)
	};

	// Opaque handle to a render-graph-managed image resource.
	// Acquired from RenderGraph::GetSwapchainColor/Depth or
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

	// Opaque handle to a render-graph-managed buffer resource.
	struct RGBuffer
	{
		static constexpr uint32_t kInvalid = ~0u;
		uint32_t id = kInvalid;

		[[nodiscard]] bool IsValid() const
		{
			return id != kInvalid;
		}
	};

	// Backward-compatible shims. Prefer gpu::ClearColor / gpu::ClearDepth.
	[[nodiscard]] inline gpu::ClearValue ClearColorValue(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f) noexcept
	{
		return gpu::ClearColor(r, g, b, a);
	}

	[[nodiscard]] inline gpu::ClearValue ClearDepthValue(float depth = 1.0f, uint32_t stencil = 0u) noexcept
	{
		return gpu::ClearDepth(depth, stencil);
	}

	// Data made available inside pass execute callbacks.
	struct PassContext
	{
		gpu::CommandList& recorder;
		gpu::Extent2D extent;
		std::uint64_t frameConstantsAddr = 0;
		std::uint32_t frameIndex = 0;
	};

	// Frame graph with pass/resource declarations and automatic image barriers.
	class RenderGraph
	{
	public:
		struct TransientImageDesc
		{
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::Extent2D extent{}; // {0,0} = match FrameTarget extent at Execute()
		};

		RenderGraph();
		~RenderGraph();

		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;
		RenderGraph(RenderGraph&&) noexcept;
		RenderGraph& operator=(RenderGraph&&) noexcept;

		void Initialize(void* device, void* allocator);
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
			~PassBuilder() = default;
			PassBuilder(PassBuilder&&) = default;
			PassBuilder(const PassBuilder&) = AE_DELETE_MSG("PassBuilder is move-only - use std::move");
			PassBuilder& operator=(PassBuilder&&) = AE_DELETE_MSG("PassBuilder is move-only - use std::move");
			PassBuilder& operator=(const PassBuilder&) = delete;

			// Declare a color attachment write.
			PassBuilder& WriteColor(RGImage image, gpu::LoadOp loadOp = gpu::LoadOp::Clear, gpu::StoreOp storeOp = gpu::StoreOp::Store, gpu::ClearValue clearValue = {});

			// Declare a depth/stencil attachment write.
			PassBuilder& WriteDepth(RGImage image, gpu::LoadOp loadOp = gpu::LoadOp::Clear, gpu::StoreOp storeOp = gpu::StoreOp::DontCare, gpu::ClearValue clearValue = {});

			// Declare a sampled texture read (graphics or compute).
			PassBuilder& ReadTexture(RGImage image);

			// Declare a storage-image read in compute.
			PassBuilder& ReadStorageImage(RGImage image);

			// Declare a storage-image write in compute.
			PassBuilder& WriteStorageImage(RGImage image);

			// Declare a storage buffer read (compute or graphics vertex/fragment).
			PassBuilder& ReadBuffer(RGBuffer buffer);

			// Declare a storage buffer write (compute only).
			PassBuilder& WriteBuffer(RGBuffer buffer);

			// Declare a storage buffer read+write (compute only).
			PassBuilder& ReadWriteBuffer(RGBuffer buffer);

			// Set graphics callback for this pass.
			PassBuilder& Execute(std::function<void(PassContext&)> fn);

			// Set compute callback for this pass.
			PassBuilder& ExecuteCompute(std::function<void(PassContext&)> fn);

			// Override pass extent (for render-to-texture and non-swapchain targets).
			PassBuilder& SetExtent(gpu::Extent2D extent);

			// Assign this pass to a specific hardware queue. Only meaningful for
			// compute passes; graphics passes always run on the graphics queue.
			PassBuilder& SetQueueClass(QueueClass qc);

			// Convenience: mark this compute pass for the async compute queue.
			PassBuilder& SetAsyncCompute()
			{
				return SetQueueClass(QueueClass::AsyncCompute);
			}

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
		// image/view are opaque Vulkan handles (VkImage/VkImageView) passed as void*.
		[[nodiscard]] RGImage RegisterImage(void* image, void* view, gpu::ImageAspect aspect = gpu::ImageAspect::Color);

		// Register an externally-owned buffer and return an RGBuffer handle.
		// buffer is an opaque Vulkan handle (VkBuffer) passed as void*.
		[[nodiscard]] RGBuffer RegisterBuffer(void* buffer);

		// Update the Vulkan buffer backing an existing RGBuffer handle.
		// Used when per-frame buffers change (e.g. triple-buffered lighting data).
		void UpdateExternalBuffer(RGBuffer buffer, void* newBuffer);

		// Create a render-graph-owned transient image.
		[[nodiscard]] RGImage CreateTransientImage(const TransientImageDesc& desc);

		// Convenience helpers for transient color/depth attachments.
		[[nodiscard]] RGImage CreateTransientColor(gpu::Format format, gpu::Extent2D extent = {}, gpu::ImageUsage extraUsage = gpu::ImageUsage::None);
		[[nodiscard]] RGImage CreateTransientDepth(gpu::Format format, gpu::Extent2D extent = {}, gpu::ImageUsage extraUsage = gpu::ImageUsage::None);

		// Ensure a transient image is registered for bindless sampled access.
		// Returns 0xFFFFFFFF when image is invalid/non-transient/not allocatable.
		// device is an opaque Vulkan device handle (VkDevice) passed as void*.
		[[nodiscard]] std::uint32_t EnsureBindlessSampled(RGImage image, BindlessManager& bindlessManager, void* device, gpu::ImageLayout descriptorLayout = gpu::ImageLayout::ShaderReadOnly);

		// Returns bindless slot for a transient image if already registered.
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(RGImage image) const;

		// Release a registered image handle from the graph.
		// For transients this also destroys owned GPU memory.
		void ReleaseImage(RGImage image);

		// Register a graphics pass.
		[[nodiscard]] PassBuilder AddPass(std::string name, std::source_location loc = std::source_location::current());

		// Register a compute pass.
		[[nodiscard]] PassBuilder AddComputePass(std::string name, std::source_location loc = std::source_location::current());

		// Register a compute pass on the async-compute queue.
		// Equivalent to AddComputePass(name).SetAsyncCompute().
		[[nodiscard]] PassBuilder AddAsyncComputePass(std::string name, std::source_location loc = std::source_location::current());

		void RemovePass(const std::string& name);
		[[nodiscard]] bool HasPass(std::string_view name) const;
		void Clear();

		// Per-frame allocation and execution statistics.
		[[nodiscard]] const FrameStats& GetFrameStats() const;

		[[nodiscard]] bool IsEmpty() const
		{
			return m_passes.empty();
		}

		struct PassInfo
		{
			std::string name;
			bool isGraphics = false;
			bool isCompute = false;
			bool isAsyncCompute = false;
			float lastCpuTimeMs = 0.f;
		};

		[[nodiscard]] std::vector<PassInfo> GetPasses() const;

		// Execute the compiled frame graph for the current frame.
		void Execute(gpu::CommandList& recorder, const FrameTarget& target, std::uint64_t frameConstantsAddr, std::uint32_t frameIndex);

		// Enable async compute scheduling. Call once after Initialize() when a
		// dedicated compute queue is available. computeQueue and queueFamily are
		// opaque Vulkan handles passed as void*.
		void EnableAsyncCompute(void* computeQueue, std::uint32_t computeQueueFamily);

		// Returns true if any compiled passes were assigned to the async compute
		// queue during the most recent Compile().
		[[nodiscard]] bool HasAsyncComputeWork() const;

		// Timeline semaphore handle (void* = VkSemaphore) and signal value that the
		// compute queue submission signals. The graphics queue submission must wait
		// on this semaphore at this value. Only valid after Execute() when
		// HasAsyncComputeWork() returns true.
		[[nodiscard]] std::uint64_t GetComputeTimelineSemaphore() const;
		[[nodiscard]] std::uint64_t GetComputeTimelineValue() const;

	private:
		static constexpr uint32_t kSwapchainColorId = 0u;
		static constexpr uint32_t kSwapchainDepthId = 1u;
		static constexpr uint32_t kFirstExternalId = 2u;
		static constexpr uint32_t kFirstTransientId = 0x40000000u;

		// Attachment reference using engine-side enums (Vulkan-free).
		struct AttachmentRef
		{
			RGImage image{};
			gpu::LoadOp loadOp = gpu::LoadOp::Clear;
			gpu::StoreOp storeOp = gpu::StoreOp::Store;
			gpu::ClearValue clearValue{};
		};

		// Per-resource access type for image barrier compilation.
		enum class ImageAccessType
		{
			SampledRead,
			StorageRead,
			StorageWrite,
		};

		// Per-resource access type for buffer barrier compilation.
		enum class BufferAccessType : uint8_t
		{
			StorageRead,
			StorageWrite,
			StorageReadWrite,
		};

		struct ImageAccessRef
		{
			RGImage image{};
			ImageAccessType type = ImageAccessType::SampledRead;
		};

		struct BufferAccessRef
		{
			RGBuffer buffer{};
			BufferAccessType type = BufferAccessType::StorageRead;
		};

		// Barrier description with engine-side enums.
		// Stage/access bits use raw uint64_t (set from VkPipelineStageFlags2 /
		// VkAccessFlags2 values at compile time) to avoid depending on Vulkan
		// types in the header.
		struct CompiledBarrier
		{
			uint32_t resourceId = 0;
			gpu::ImageLayout oldLayout = gpu::ImageLayout::Undefined;
			gpu::ImageLayout newLayout = gpu::ImageLayout::Undefined;
			std::uint64_t srcStage = 0;
			std::uint64_t srcAccess = 0;
			std::uint64_t dstStage = 0;
			std::uint64_t dstAccess = 0;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			bool isWAR = false;
		};

		// Buffer barrier — image-free, just stage/access tracking.
		struct CompiledBufferBarrier
		{
			uint32_t resourceId = 0;
			std::uint64_t srcStage = 0;
			std::uint64_t srcAccess = 0;
			std::uint64_t dstStage = 0;
			std::uint64_t dstAccess = 0;
			bool isWAR = false;
		};

		// A set of wait barriers sharing a single event.
		struct CompiledWait
		{
			std::uint32_t eventIndex = UINT32_MAX;
			std::vector<CompiledBarrier> barriers;
		};

		struct CompiledPass
		{
			std::size_t passIndex = 0;
			QueueClass queueClass = QueueClass::Graphics;
			std::vector<CompiledBarrier> preBarriers;          // non-split barriers
			std::vector<CompiledBufferBarrier> bufferBarriers; // buffer barriers for this pass
			std::vector<CompiledBarrier> signalBarriers;       // emitted as vkCmdSetEvent2 at end of producer
			std::uint32_t splitEventIndex = UINT32_MAX;        // event this pass signals (VkEvent index in storage)
			std::vector<CompiledWait> waits;                   // events/barriers to wait on at start of consumer
		};

		// Tracking state for barrier compilation (engine-side enums + raw bits).
		struct ResourceState
		{
			gpu::ImageLayout layout = gpu::ImageLayout::Undefined;
			std::uint64_t writeStage = 0;
			std::uint64_t writeAccess = 0;
			std::uint64_t readStages = 0;
		};

		// Tracking state for buffer barriers.
		struct BufferState
		{
			std::uint64_t writeStage = 0;
			std::uint64_t writeAccess = 0;
			std::uint64_t readStages = 0;
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
			QueueClass queueClass = QueueClass::Graphics;
			std::vector<AttachmentRef> colorWrites;
			std::optional<AttachmentRef> depthWrite;
			std::vector<ImageAccessRef> imageAccesses;
			std::vector<BufferAccessRef> bufferAccesses;
			std::function<void(PassContext&)> execute;
			std::optional<gpu::Extent2D> extentOverride;
			float lastCpuTimeMs = 0.f;
#ifndef NDEBUG
			std::source_location declaredAt;
#endif
		};

		// External image entry (opaque handles).
		struct ExternalImageEntry
		{
			void* image = nullptr;
			void* view = nullptr;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
		};

		void Compile();

#ifndef NDEBUG
		struct BarrierIssue
		{
			enum class Kind
			{
				Redundant,
				MissingAccessMask,
				NeedsTopOfPipe,
				VertexSamplingGap,
			};

			Kind kind;
			std::size_t passIndex;
			uint32_t resourceId;
			std::string message;
		};

		[[nodiscard]] std::vector<BarrierIssue> EvaluateBarriers() const;
#endif

		[[nodiscard]] bool IsTransientId(uint32_t resourceId) const
		{
			return resourceId >= kFirstTransientId;
		}

		[[nodiscard]] uint32_t ExternalIndex(uint32_t resourceId) const
		{
			return resourceId - kFirstExternalId;
		}

		[[nodiscard]] uint32_t TransientIndex(uint32_t resourceId) const
		{
			return resourceId - kFirstTransientId;
		}

		// Opaque storage for all Vulkan-internal state.
		std::unique_ptr<RenderGraphStorage> m_storage;

		// Pass graph state (Vulkan-free).
		std::vector<PassRecord> m_passes;
		std::vector<CompiledPass> m_compiled;
		std::vector<ExternalImageEntry> m_externalImages;
		std::vector<void*> m_externalBuffers;
		std::unordered_map<uint32_t, ResourceState> m_lastImageStates;
		std::unordered_map<uint32_t, BufferState> m_lastBufferStates;
		std::uint32_t m_frameIndex = 0;
		TracyVkCtx m_tracyVkCtx = nullptr;
		bool m_compileDirty = true;
		bool m_asyncComputeEnabled = false;
	};
} // namespace aether
