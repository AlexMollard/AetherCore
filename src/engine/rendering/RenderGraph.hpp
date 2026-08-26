#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <source_location>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/Semaphore.hpp"
#include "rendering/FrameBlackboard.hpp"
#include "rendering/RenderGraphTypes.hpp"
#include "rendering/TransientHeapPacker.hpp"

namespace aether
{
	class BindlessManager;
	class DiagnosticEngine;

	struct RenderGraphStorage;
	struct FrameStats;

	class RenderGraph
	{
	public:
		struct TransientImageDesc
		{
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageUsage usage = gpu::ImageUsage::None;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::Extent2D extent;
		};

		struct FrameProductRef
		{
			std::type_index type = std::type_index(typeid(void));
			std::string name;
			std::string typeName;
		};

		template<typename T>
		[[nodiscard]] static FrameProductRef Product(std::string_view name)
		{
			return FrameProductRef{.type = std::type_index(typeid(T)), .name = std::string{name}, .typeName = typeid(T).name()};
		}

		struct FullscreenPassDesc
		{
			std::string name;
			RGImage color{};
			gpu::Extent2D extent;
			gpu::LoadOp loadOp = gpu::LoadOp::Load;
			gpu::StoreOp storeOp = gpu::StoreOp::Store;
			gpu::ClearValue clearValue = ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f);
			std::vector<FrameProductRef> consumes;
			std::vector<FrameProductRef> produces;
		};

		struct DepthOnlyPassDesc
		{
			std::string name;
			RGImage depth{};
			PreparedDrawList draws{};
			gpu::Extent2D extent;
			gpu::LoadOp loadOp = gpu::LoadOp::Clear;
			gpu::StoreOp storeOp = gpu::StoreOp::Store;
			gpu::ClearValue clearValue = ClearDepthValue(1.0f);
			std::vector<FrameProductRef> consumes;
			std::vector<FrameProductRef> produces;
		};

		struct DrawQueuePassDesc
		{
			std::string name;
			RGImage color{};
			RGImage depth{};
			PreparedDrawList draws{};
			gpu::Extent2D extent;
			gpu::LoadOp colorLoadOp = gpu::LoadOp::Load;
			gpu::StoreOp colorStoreOp = gpu::StoreOp::Store;
			gpu::LoadOp depthLoadOp = gpu::LoadOp::Load;
			gpu::StoreOp depthStoreOp = gpu::StoreOp::Store;
			gpu::ClearValue depthClearValue = ClearDepthValue(1.0f);
			std::vector<FrameProductRef> consumes;
			std::vector<FrameProductRef> produces;
		};

		struct QueuePreparePassDesc
		{
			std::string name;
			PreparedDrawList produces{};
			std::string sideEffectReason = "prepares draw queue state";
			bool keepOnGraphicsQueue = true;
			std::vector<FrameProductRef> consumesProducts;
			std::vector<FrameProductRef> producesProducts;
		};

		struct ComputeImagePassDesc
		{
			std::string name;
			RGImage image{};
			gpu::Extent2D extent;
			bool readsStorageImage = false;
			bool writesStorageImage = true;
			QueueClass queueClass = QueueClass::Graphics;
			std::vector<FrameProductRef> consumes;
			std::vector<FrameProductRef> produces;
		};

		struct ComputeBufferPassDesc
		{
			std::string name;
			std::vector<RGBuffer> reads;
			std::vector<RGBuffer> writes;
			std::vector<RGBuffer> readWrites;
			gpu::Extent2D extent;
			QueueClass queueClass = QueueClass::Graphics;
			std::vector<FrameProductRef> consumes;
			std::vector<FrameProductRef> produces;
		};

		struct ShaderResourceBinding
		{
			FrameProductRef product;
			FrameResourceId resourceId = FrameResourceId::Count;
			std::function<void(const FrameBlackboard& blackboard, std::span<ResourceEntry> entries)> writeResourceTable;
		};

		RenderGraph();
		~RenderGraph();

		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;
		RenderGraph(RenderGraph&&) noexcept;
		RenderGraph& operator=(RenderGraph&&) noexcept;

		void Initialize(gpu::Device device, gpu::Allocator allocator);
		void SetVulkanContext(class VulkanContext* ctx);

		void SetDiagnosticEngine(class DiagnosticEngine* de)
		{
			m_diagnosticEngine = de;
		}

		void Shutdown();

		// Begin a new frame - must be called before Execute() to process
		void BeginFrame(std::uint32_t frameIndex);

		class PassBuilder
		{
		public:
			~PassBuilder() = default;
			PassBuilder(PassBuilder&&) = default;
			PassBuilder(const PassBuilder&) = AE_DELETE_MSG("PassBuilder is move-only - use std::move");
			PassBuilder& operator=(PassBuilder&&) = AE_DELETE_MSG("PassBuilder is move-only - use std::move");
			PassBuilder& operator=(const PassBuilder&) = delete;

			PassBuilder& WriteColor(RGImage image, gpu::LoadOp loadOp = gpu::LoadOp::Clear, gpu::StoreOp storeOp = gpu::StoreOp::Store, gpu::ClearValue clearValue = {});

			PassBuilder& WriteDepth(RGImage image, gpu::LoadOp loadOp = gpu::LoadOp::Clear, gpu::StoreOp storeOp = gpu::StoreOp::DontCare, gpu::ClearValue clearValue = {});

			PassBuilder& ReadTexture(RGImage image);

			PassBuilder& ReadStorageImage(RGImage image);

			PassBuilder& WriteStorageImage(RGImage image);

			PassBuilder& ReadBuffer(RGBuffer buffer);

			PassBuilder& WriteBuffer(RGBuffer buffer);

			PassBuilder& ReadWriteBuffer(RGBuffer buffer);

			PassBuilder& ReadImageTransfer(RGImage image);

			PassBuilder& WriteImageTransfer(RGImage image);

			PassBuilder& ReadBufferTransfer(RGBuffer buffer);

			PassBuilder& WriteBufferTransfer(RGBuffer buffer);

			PassBuilder& Execute(std::function<void(PassContext&)> fn);

			PassBuilder& ExecuteCompute(std::function<void(PassContext&)> fn);

			PassBuilder& OnDebugDisabled(std::function<void(PassContext&)> fn);

			PassBuilder& SetExtent(gpu::Extent2D extent);

			PassBuilder& SetQueueClass(QueueClass qc);

			PassBuilder& DisableAsyncCompute();

			// Declare non-resource work that must not be culled or reordered
			PassBuilder& HasSideEffects(std::string reason = {});

			// name must match the stable user-facing pass name, e.g. "$CullDraws".
			PassBuilder& DependsOn(std::string passNamePrefix);

			PassBuilder& ProducesDrawList(PreparedDrawList drawList);
			PassBuilder& ConsumesDrawList(PreparedDrawList drawList);

			PassBuilder& ProducesProductRef(const FrameProductRef& product);
			PassBuilder& ConsumesProductRef(const FrameProductRef& product);

			template<typename T>
			PassBuilder& ProducesProduct(std::string_view name)
			{
				return ProducesProductRef(RenderGraph::Product<T>(name));
			}

			template<typename T>
			PassBuilder& ConsumesProduct(std::string_view name)
			{
				return ConsumesProductRef(RenderGraph::Product<T>(name));
			}

			template<typename T>
			PassBuilder& ConsumeTextureProduct(std::string_view name, FrameResourceId resourceId)
			{
				ConsumesProduct<T>(name);

				if (const T* product = m_graph.m_blackboard.TryGet<T>(name))
				{
					std::vector<RGImage> sampledImages;
					FrameProductShaderResources<T>::AppendSampledImages(*product, sampledImages);
					for (const RGImage image: sampledImages)
					{
						ReadTexture(image);
					}
				}

				std::string productName{name};
				ShaderResourceBinding binding{
				        .product = RenderGraph::Product<T>(name),
				        .resourceId = resourceId,
				        .writeResourceTable =
				                [productName = std::move(productName), resourceId](const FrameBlackboard& blackboard, std::span<ResourceEntry> entries)
				        {
					        if (const T* product = blackboard.TryGet<T>(productName))
					        {
						        FrameProductShaderResources<T>::WriteResourceTable(*product, resourceId, entries);
					        }
				        },
				};

				const std::scoped_lock lock(m_graph.m_debugStateMutex);
				m_graph.m_passes[m_passIndex].shaderResourceBindings.push_back(std::move(binding));
				return *this;
			}

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

		[[nodiscard]] static RGImage GetSwapchainColor()
		{
			return RGImage{kSwapchainColorId};
		}

		[[nodiscard]] static RGImage GetSwapchainDepth()
		{
			return RGImage{kSwapchainDepthId};
		}

		[[nodiscard]] RGImage RegisterImage(gpu::Image image, gpu::ImageView view, gpu::ImageAspect aspect = gpu::ImageAspect::Color);

		[[nodiscard]] RGBuffer RegisterBuffer(gpu::Buffer buffer);

		// Graph-owned scratch storage. The graph decides where it lives and may hand the
		// same memory to another transient whose lifetime does not overlap, so the buffer
		// and its device address are only valid inside a pass Execute callback and only for
		// the frame that reads them - never cache either across frames.
		[[nodiscard]] RGBuffer CreateTransientBuffer(gpu::DeviceSize size, gpu::BufferUsage usage);
		void ReleaseBuffer(RGBuffer buffer);
		[[nodiscard]] gpu::Buffer ResolveBuffer(RGBuffer buffer) const;
		[[nodiscard]] gpu::DeviceAddress GetBufferAddress(RGBuffer buffer) const;

		void UpdateExternalBuffer(RGBuffer buffer, gpu::Buffer newBuffer);

		[[nodiscard]] RGImage CreateTransientImage(const TransientImageDesc& desc);

		// The image currently backing a resource, for the passes that name it directly rather
		// than through an attachment - transfers, mostly. Same rule as ResolveBuffer: a pooled
		// transient's image changes whenever the graph is rebuilt, so resolve inside Execute
		// and never cache the result.
		[[nodiscard]] gpu::Image ResolveImage(RGImage image) const;

		[[nodiscard]] RGImage CreateTransientColor(gpu::Format format, gpu::Extent2D extent = {}, gpu::ImageUsage extraUsage = gpu::ImageUsage::None);
		[[nodiscard]] RGImage CreateTransientDepth(gpu::Format format, gpu::Extent2D extent = {}, gpu::ImageUsage extraUsage = gpu::ImageUsage::None);

		[[nodiscard]] std::uint32_t EnsureBindlessSampled(RGImage image, gpu::ImageLayout descriptorLayout = gpu::ImageLayout::ShaderReadOnly);

		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(RGImage image) const;

		void ReleaseImage(RGImage image);

		[[nodiscard]] PassBuilder AddPass(std::string name, std::source_location loc = std::source_location::current());

		[[nodiscard]] PassBuilder AddComputePass(std::string name, std::source_location loc = std::source_location::current());

		[[nodiscard]] PassBuilder AddFullscreenPass(FullscreenPassDesc desc, std::source_location loc = std::source_location::current());
		[[nodiscard]] PassBuilder AddDepthOnlyPass(DepthOnlyPassDesc desc, std::source_location loc = std::source_location::current());
		[[nodiscard]] PassBuilder AddDrawQueuePass(DrawQueuePassDesc desc, std::source_location loc = std::source_location::current());
		[[nodiscard]] PassBuilder AddQueuePreparePass(QueuePreparePassDesc desc, std::source_location loc = std::source_location::current());
		[[nodiscard]] PassBuilder AddComputeImagePass(ComputeImagePassDesc desc, std::source_location loc = std::source_location::current());
		[[nodiscard]] PassBuilder AddComputeBufferPass(ComputeBufferPassDesc desc, std::source_location loc = std::source_location::current());

		[[nodiscard]] PreparedDrawList CreatePreparedDrawList(std::string name);
		void RemovePreparedDrawList(PreparedDrawList drawList);

		void RemovePass(const std::string& name);
		void Clear();

		[[nodiscard]] const FrameStats& GetFrameStats() const;

		[[nodiscard]] const FrameResourceContext& GetLastFrameContext() const
		{
			return m_lastFrameContext;
		}

		[[nodiscard]] FrameBlackboard& GetBlackboard()
		{
			return m_blackboard;
		}

		[[nodiscard]] const FrameBlackboard& GetBlackboard() const
		{
			return m_blackboard;
		}

		[[nodiscard]] bool IsEmpty() const
		{
			return m_passes.empty();
		}

		struct PassInfo
		{
			struct ResourceAccessInfo
			{
				enum class Kind
				{
					Image,
					Buffer,
				};

				Kind kind = Kind::Image;
				std::uint32_t id = 0;
				std::string usage;
				bool writes = false;
			};

			std::size_t index = 0;
			std::size_t compiledIndex = 0;
			std::string name;
			std::string declaredFile;
			std::uint_least32_t declaredLine = 0;
			bool isGraphics = false;
			bool isCompute = false;
			bool isAsyncCompute = false;
			bool isCompiled = false;
			bool isCulled = false;
			bool isDebugDisabled = false;
			bool hasSideEffects = false;
			std::string sideEffectReason;
			std::vector<std::string> logicalDependencies;
			std::vector<std::string> producedDrawLists;
			std::vector<std::string> consumedDrawLists;
			std::vector<std::string> producedFrameProducts;
			std::vector<std::string> consumedFrameProducts;
			std::vector<std::string> contractWarnings;
			bool hasDepthWrite = false;
			std::uint32_t colorWriteCount = 0;
			std::uint32_t imageAccessCount = 0;
			std::uint32_t bufferAccessCount = 0;
			std::uint32_t preBarrierCount = 0;
			std::uint32_t bufferBarrierCount = 0;
			std::uint32_t waitCount = 0;
			std::uint32_t signalBarrierCount = 0;
			std::uint32_t splitEventIndex = UINT32_MAX;
			std::optional<gpu::Extent2D> extentOverride;
			std::vector<ResourceAccessInfo> resources;
			float lastCpuTimeMs = 0.f;
			float lastGpuTimeMs = 0.f;
		};

		[[nodiscard]] std::vector<PassInfo> GetPasses() const;
		void SetPassDebugDisabled(std::string_view name, bool disabled);
		[[nodiscard]] bool IsPassDebugDisabled(std::string_view name) const;
		void ClearDebugDisabledPasses();
		void PopulateResourceTable(std::span<ResourceEntry> entries) const;

		void Execute(gpu::CommandList& cmdList, const FrameResourceContext& frame);

		void EnableAsyncCompute(gpu::Queue computeQueue, std::uint32_t computeQueueFamily);

		[[nodiscard]] bool HasAsyncComputeWork() const;

		// compute queue submission signals. The graphics queue submission must wait
		[[nodiscard]] gpu::TimelineSemaphoreHandle GetComputeTimelineSemaphore() const;
		[[nodiscard]] std::uint64_t GetComputeTimelineValue() const;

		// Must be called after Execute() and before the graphics queue submission.
		void SubmitComputeWork(std::uint32_t frameIndex);

	private:
		static constexpr uint32_t kSwapchainColorId = 0u;
		static constexpr uint32_t kSwapchainDepthId = 1u;
		static constexpr uint32_t kFirstExternalId = 2u;
		static constexpr uint32_t kFirstExternalBufferId = 0x20000000u;
		static constexpr uint32_t kFirstTransientId = 0x40000000u;
		static constexpr uint32_t kFirstTransientBufferId = 0x60000000u;

		struct AttachmentRef
		{
			RGImage image{};
			gpu::LoadOp loadOp = gpu::LoadOp::Clear;
			gpu::StoreOp storeOp = gpu::StoreOp::Store;
			gpu::ClearValue clearValue{};
		};

		enum class ImageAccessType
		{
			SampledRead,
			StorageRead,
			StorageWrite,
			TransferRead,
			TransferWrite,
		};

		enum class BufferAccessType : uint8_t
		{
			StorageRead,
			StorageWrite,
			StorageReadWrite,
			TransferRead,
			TransferWrite,
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

		struct CompiledBufferBarrier
		{
			uint32_t resourceId = 0;
			std::uint64_t srcStage = 0;
			std::uint64_t srcAccess = 0;
			std::uint64_t dstStage = 0;
			std::uint64_t dstAccess = 0;
			bool isWAR = false;
		};

		struct CompiledWait
		{
			std::uint32_t eventIndex = UINT32_MAX;
			std::vector<CompiledBarrier> barriers;
		};

		struct CompiledPass
		{
			std::size_t passIndex = 0;
			QueueClass queueClass = QueueClass::Graphics;
			std::vector<CompiledBarrier> preBarriers;
			std::vector<CompiledBufferBarrier> bufferBarriers;
			std::vector<CompiledBarrier> signalBarriers;
			std::uint32_t splitEventIndex = UINT32_MAX;
			std::vector<CompiledWait> waits;
		};

		struct ResourceState
		{
			gpu::ImageLayout layout = gpu::ImageLayout::Undefined;
			std::uint64_t writeStage = 0;
			std::uint64_t writeAccess = 0;
			std::uint64_t readStages = 0;
		};

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
			bool allowAsyncCompute = true;
			std::vector<AttachmentRef> colorWrites;
			std::optional<AttachmentRef> depthWrite;
			std::vector<ImageAccessRef> imageAccesses;
			std::vector<BufferAccessRef> bufferAccesses;
			std::function<void(PassContext&)> execute;
			std::function<void(PassContext&)> debugDisabledExecute;
			std::optional<gpu::Extent2D> extentOverride;
			float lastCpuTimeMs = 0.f;
			float lastGpuTimeMs = 0.f;
			bool debugDisabled = false;
			bool hasSideEffects = false;
			std::string sideEffectReason;
			std::vector<std::string> logicalDependencies;
			std::vector<PreparedDrawList> producedDrawLists;
			std::vector<PreparedDrawList> consumedDrawLists;
			std::vector<FrameProductRef> producedFrameProducts;
			std::vector<FrameProductRef> consumedFrameProducts;
			std::vector<ShaderResourceBinding> shaderResourceBindings;
			std::vector<std::string> contractWarnings;
#ifndef NDEBUG
			std::source_location declaredAt;
#endif
		};

		struct PreparedDrawListRecord
		{
			std::string name;
			std::size_t producerPass = std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> consumerPasses;
			bool retired = false;
		};

		struct ExternalImageEntry
		{
			gpu::Image image = nullptr;
			gpu::ImageView view = nullptr;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
		};

		void Compile();
		void RebuildPreparedDrawListLinks();

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

		// Per-pass GPU timing. Timestamps are written either side of every pass into a
		// pool owned by the frame slot, and read back one full cycle later, when that
		// slot's fence has already been waited on - so the results are free and never
		// stall. Whole-frame fps is far too noisy to optimise against (measured at 16ms
		// median with a 6ms standard deviation in the editor), which left the renderer
		// with no usable cost signal at all; this is that signal.
		struct GpuTimingFrame
		{
			gpu::QueryPool pool = nullptr;
			std::uint32_t capacity = 0;                 // queries allocated, = 2 * passes
			std::uint32_t used = 0;                     // queries written this frame
			std::vector<std::uint32_t> timedPasses;     // pass index per timed slot, in order
			bool pending = false;                       // has results waiting to be read
		};

		void ResolveGpuTimings(std::uint32_t frameSlot);
		void ResetGpuTimings(gpu::CommandList& cmdList, std::uint32_t frameSlot, std::uint32_t passCount);
		void DestroyGpuTimings();

		gpu::Device m_timingDevice = nullptr;
		class VulkanContext* m_timingContext = nullptr;
		// Nanoseconds per timestamp tick. Zero means the device cannot time, and every
		// timing path below turns into a no-op.
		float m_timestampPeriodNs = 0.0f;
		std::array<GpuTimingFrame, kMaxFramesInFlight> m_gpuTiming{};

		[[nodiscard]] static bool IsTransientId(uint32_t resourceId)
		{
			return resourceId >= kFirstTransientId && resourceId < kFirstTransientBufferId;
		}

		[[nodiscard]] static bool IsTransientBufferId(uint32_t resourceId)
		{
			return resourceId >= kFirstTransientBufferId;
		}

		[[nodiscard]] static uint32_t ExternalIndex(uint32_t resourceId)
		{
			return resourceId - kFirstExternalId;
		}

		[[nodiscard]] static uint32_t ExternalBufferIndex(uint32_t resourceId)
		{
			return resourceId - kFirstExternalBufferId;
		}

		[[nodiscard]] static uint32_t TransientIndex(uint32_t resourceId)
		{
			return resourceId - kFirstTransientId;
		}

		[[nodiscard]] static uint32_t TransientBufferIndex(uint32_t resourceId)
		{
			return resourceId - kFirstTransientBufferId;
		}

		void ComputeTransientLifetimes(const std::vector<std::size_t>& sortedIndices, const std::vector<bool>& culled);

		// True when the resource's memory may be shared with another transient, which is
		// what makes its cross-frame contents meaningless and its first use a full discard.
		[[nodiscard]] bool IsPoolableTransient(uint32_t resourceId) const;
		[[nodiscard]] std::uint64_t FirstUseSrcStage(uint32_t resourceId) const;
		[[nodiscard]] std::uint64_t FirstUseSrcAccess(uint32_t resourceId) const;

		std::unique_ptr<RenderGraphStorage> m_storage;
		mutable std::mutex m_debugStateMutex;

		class DiagnosticEngine* m_diagnosticEngine = nullptr;

		std::vector<PassRecord> m_passes;
		std::vector<CompiledPass> m_compiled;
		std::vector<bool> m_lastCulledPasses;
		std::vector<ExternalImageEntry> m_externalImages;
		std::vector<gpu::Buffer> m_externalBuffers;
		std::vector<PreparedDrawListRecord> m_preparedDrawLists;
		FrameBlackboard m_blackboard;
		FrameResourceContext m_lastFrameContext{};
		std::unordered_map<uint32_t, ResourceState> m_lastImageStates;
		std::unordered_map<uint32_t, BufferState> m_lastBufferStates;
		// Indexed by transient slot. Recomputed by Compile(), consumed by the heap plan.
		std::vector<TransientLifetime> m_transientImageLifetimes;
		std::vector<TransientLifetime> m_transientBufferLifetimes;
		std::uint32_t m_frameIndex = 0;
		bool m_compileDirty = true;
		bool m_asyncComputeEnabled = false;
	};
} // namespace aether
