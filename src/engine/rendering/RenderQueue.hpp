#pragma once

#include <functional>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include "gpu/CommandList.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

#include "animation/AnimationDatabase.hpp"
#include "rendering/GpuContracts.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether
{
	class AnimationBlendSystem;
	class AnimationIkSystem;
	class AnimationRootMotionSystem;
	class GraphicsPipeline;
	class Mesh;

	// Shared compute pipelines used by every RenderQueue instance.
	// Create one, initialize it once, then pass a const reference to each RenderQueue::Initialize().
	struct RenderQueueSharedPipelines
	{
		gpu::PipelineHandle skinCopy;
		gpu::PipelineHandle animSample;
		gpu::PipelineHandle nodeFlatten;
		gpu::PipelineHandle poseInit;
		gpu::PipelineHandle animBlend;
		gpu::PipelineHandle ikSolve;

		void Initialize(gpu::Device device, gpu::PipelineCache pipelineCache);
		void Shutdown(gpu::Device device);
	};

	// Per-draw submission payload.
	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr; // must be indexed; null draws are not supported
		std::uint32_t instanceCount = 1;
		glm::mat4 modelMatrix{1.0f};               // per-object world transform
		std::uint32_t materialIndex = 0xFFFFFFFFu; // index into MaterialBuffer; 0xFFFF... = fallback
		std::int32_t skinIndex = -1;               // skin index in AnimationDatabase; -1 = not skinned
		std::uint32_t skinJointCount = 0;          // number of joints in the skin
		std::uint32_t animClipIndex = 0;           // active clip for GPU sampling
		float animTime = 0.0f;                     // active clip time for GPU sampling
		glm::vec4 worldBoundingSphere{};           // xyz=world center, w=radius; w<=0 = skip culling
		const AnimationDatabase* animDb = nullptr; // per-draw animation database for GPU sampling
		std::uint32_t animDbGeneration = 0;        // generation counter for validity check
		std::uint32_t meshGeneration = 0;          // generation counter for mesh validity check
	};

	// Collects draws, runs cull/animation compute, then emits indirect draws.
	// Configuration struct for Initialize - replaces multiple parameters.
	// maxAnimationDraws controls animation pool sizes separately from total draw capacity.
	// Pass 0 to disable all animation/skin buffers (for non-skinned queues).
	// UINT32_MAX (default) derives a sane cap from total draws using kDefaultMaxAnimationDraws.
	// outputDrawCapacity overrides per-frame indirect output buffer capacity (0 = equals maxDraws).
	// Use e.g. maxDraws * 3 for multi-frustum shadow queues writing 3 cascade regions.
	struct RenderQueueConfig
	{
		std::uint32_t maxDraws = 8192;
		std::uint32_t maxBatches = 1024;
		std::uint32_t maxAnimationDraws = UINT32_MAX;
		std::uint32_t outputDrawCapacity = 0;
	};

	class RenderQueue
	{
	public:
		static constexpr std::uint32_t kFramesInFlight = Swapchain::kMaxFramesInFlight;
		static constexpr std::uint32_t kDefaultMaxAnimationDraws = 1024u;

		RenderQueue() = default;

		~RenderQueue()
		{
			Shutdown();
		}

		RenderQueue(const RenderQueue&) = delete;
		RenderQueue& operator=(const RenderQueue&) = delete;
		RenderQueue(RenderQueue&&) = delete;
		RenderQueue& operator=(RenderQueue&&) = delete;

		// Initialize with configuration struct.
		void Initialize(gpu::Device device, gpu::Allocator allocator, const RenderQueueSharedPipelines& pipelines, const RenderQueueConfig& config = {});
		void Shutdown();

		// Optional animation database for GPU sampling.
		void SetAnimationDatabase(const AnimationDatabase* db)
		{
			m_animationDb = db;
		}

		// Select the frame slot used by Submit.
		void SetWriteSlot(std::uint32_t slot)
		{
			m_writeSlot = slot;
		}

		void Submit(const DrawCommand& cmd);

		void SetDebugForceVisible(bool enabled)
		{
			m_debugForceVisible = enabled;
		}

		[[nodiscard]] bool IsDebugForceVisible() const
		{
			return m_debugForceVisible;
		}

		void SetDebugBypassIndirect(bool enabled)
		{
			m_debugBypassIndirect = enabled;
		}

		[[nodiscard]] bool IsDebugBypassIndirect() const
		{
			return m_debugBypassIndirect;
		}

		void SetDebugDisableAnimation(bool enabled)
		{
			m_debugDisableAnimation = enabled;
		}

		[[nodiscard]] bool IsDebugDisableAnimation() const
		{
			return m_debugDisableAnimation;
		}

		void SetDebugAnimPassMask(std::uint32_t mask)
		{
			m_debugAnimPassMask = mask;
		}

		// Log all skin/animation job parameters for the next N frames to the engine log.
		// Use to verify addresses, counts, and indices are sane before they hit the GPU.
		void SetDebugLogSkinJobs(std::uint32_t frameCount)
		{
			m_debugLogSkinJobsFramesLeft = frameCount;
		}

		// Optional Tracy GPU context for CPU-correlated GPU timeline zones.
		// Wired through the engine-side GpuProfiler singleton - no
		// Tracy type or `Vk*` token is exposed on the public API.

		// Optional animation extension systems. When set, the blend/IK/root-motion
		// passes are dispatched after the standard animation pipeline.
		void SetAnimationBlendSystem(AnimationBlendSystem* sys)
		{
			m_animationBlendSystem = sys;
		}

		void SetAnimationIkSystem(AnimationIkSystem* sys)
		{
			m_animationIkSystem = sys;
		}

		void SetRootMotionSystem(AnimationRootMotionSystem* sys)
		{
			m_rootMotionSystem = sys;
		}

		// Hips node index for root motion copy: copy-buffer reads from
		// global transforms buffer at (hipNodeIdx * 64) bytes offset.
		void SetHipsNodeIndex(std::uint32_t hipsNodeIdx)
		{
			m_hipsNodeIdx = hipsNodeIdx;
		}

		// Write inputs and dispatch animation/cull compute.
		// For multi-frustum queues (outputDrawCapacity > maxDraws), call
		// SetMultiCullFrameAddrs() beforehand to supply the 3 cascade frame
		// constants addresses; the computePipeline/layout must then be compatible
		// with CullMultiPushConstants.
		void PrepareAndDispatch(gpu::CommandList& cmd, gpu::DeviceAddress frameAddr, gpu::Pipeline computePipeline, gpu::PipelineLayout computeLayout, std::uint32_t frameIndex);

		// For multi-frustum queues: provides the 3 cascade frame constant BDAs
		// used by PrepareAndDispatch to build CullMultiPushConstants.
		void SetMultiCullFrameAddrs(const gpu::DeviceAddress addrs[3])
		{
			m_multiFrameAddrs[0] = addrs[0];
			m_multiFrameAddrs[1] = addrs[1];
			m_multiFrameAddrs[2] = addrs[2];
		}

		[[nodiscard]] std::uint32_t GetMaxDraws() const
		{
			return m_maxDraws;
		}

		[[nodiscard]] std::uint32_t GetMaxSkinJoints() const
		{
			return m_maxSkinJoints;
		}

		[[nodiscard]] gpu::DeviceAddress GetSkinPaletteBufferAddress() const
		{
			// Per-slot device-local buffer; return slot 0's address as a
			// representative handle (used only for diagnostic logging).
			return m_skinPalette[0].address;
		}

		// Emit graphics draws from indirect output.
		// cascadeOffset is added to the output buffer offset (in gpu::DrawIndexedIndirectCommand units);
		// used by multi-frustum queues to select one cascade's output region.
		void FlushDraw(gpu::CommandList& cmd, gpu::DescriptorSet bindlessSet = nullptr, gpu::DescriptorSet lightingSet = nullptr, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0);
		void FlushDrawPush(gpu::CommandList& cmd, gpu::DescriptorSet bindlessSet, const std::function<void(gpu::CommandList&, gpu::PipelineLayout)>& pushLightingFn, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0);

		// Same as FlushDraw but overrides the frame constants BDA in push constants
		// with overrideFrameAddr. Used for rendering the same geometry from multiple POVs
		// (e.g., local shadow atlas where each light has a different VP matrix).
		void FlushDrawWithFrameAddr(gpu::CommandList& cmd, gpu::DescriptorSet bindlessSet, gpu::DescriptorSet lightingSet, gpu::DeviceAddress overrideFrameAddr, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0);

		// Clear queued commands for a frame slot.
		void Clear(std::uint32_t slot);

		[[nodiscard]] bool IsEmpty(std::uint32_t slot) const;

	private:
		// Per-frame queued draw commands.
		std::array<std::vector<DrawCommand>, kFramesInFlight> m_commandSlots;
		std::uint32_t m_writeSlot = 0; // set by game thread via SetWriteSlot()

		// ------------------------------------------------------------------------
		// Buffer storage
		//
		// All buffers are owned by gpu::ResourceRegistry and live as
		// gpu::BufferHandle slots (8 bytes each, generation-checked). Per-frame
		// data is mirrored by std::array<Handle, kFramesInFlight>. The cached
		// pointers / device-addresses are refreshed after CreateMappedBuffer /
		// ResolveBuffer so the hot path doesn't need to re-resolve every frame.
		// ------------------------------------------------------------------------

		// Per-frame CPU-written mapped SSBOs.
		struct MappedPerFrame
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;         // CPU write pointer
			gpu::DeviceAddress address = 0; // GPU read pointer
		};

		std::array<MappedPerFrame, kFramesInFlight> m_instanceData;
		std::array<MappedPerFrame, kFramesInFlight> m_cullInput;
		std::array<MappedPerFrame, kFramesInFlight> m_batchDesc;
		std::array<MappedPerFrame, kFramesInFlight> m_skinCopyJobs;
		std::array<MappedPerFrame, kFramesInFlight> m_animationSampleJobs;

		// Per-frame device-local buffers (GPU-written outputs, no mapped ptr).
		struct DevicePerFrame
		{
			gpu::BufferHandle handle{};
			gpu::DeviceAddress address = 0;
		};

		std::array<DevicePerFrame, kFramesInFlight> m_outputIndirect;
		std::array<DevicePerFrame, kFramesInFlight> m_sampledPoses;
		std::array<DevicePerFrame, kFramesInFlight> m_nodeGlobalTransforms;
		std::array<DevicePerFrame, kFramesInFlight> m_skinPalette;

		// CPU-write typed view cached on Init for hot-path access.
		DrawContracts::InstanceData* m_instanceDataMapped = nullptr;
		CullContracts::DrawInput* m_cullInputMapped = nullptr;
		CullContracts::Batch* m_batchDescMapped = nullptr;
		AnimationContracts::SkinCopyJob* m_skinCopyJobsMapped = nullptr;
		AnimationContracts::AnimatorSampleJob* m_animationSampleJobsMapped = nullptr;

		std::uint32_t m_maxDraws = 0;
		std::uint32_t m_outputDrawCapacity = 0; // indirect buffer capacity per frame slot (defaults to m_maxDraws)
		std::uint32_t m_maxBatches = 0;
		std::uint32_t m_maxAnimationDraws = 0;
		std::uint32_t m_maxSkinJoints = 0;
		std::uint32_t m_maxSampledPoses = 0;

		// Batch metadata consumed by FlushDraw.
		struct BatchRenderInfo
		{
			const GraphicsPipeline* pipeline = nullptr;
			const Mesh* mesh = nullptr;
			std::uint32_t meshGeneration = 0;
			std::uint32_t outputStart = 0; // first slot in output indirect buffer
			std::uint32_t drawCount = 0;   // capacity = max surviving draws
		};

		std::vector<BatchRenderInfo> m_batchRenderInfos;

		// Cached per-frame addresses/state for FlushDraw.
		gpu::DeviceAddress m_multiFrameAddrs[3] = {};
		gpu::DeviceAddress m_cachedFrameAddr = 0;
		gpu::DeviceAddress m_cachedInstanceDataAddr = 0;         // BDA of DrawContracts::InstanceData[0] for current frame slot
		gpu::DeviceAddress m_cachedSkinPaletteAddr = 0;          // BDA of global skin palette mat4[0] for current frame slot
		gpu::DeviceAddress m_cachedNodeGlobalTransformsAddr = 0; // BDA of per-node global transforms for current frame slot
		gpu::DeviceAddress m_cachedDrawBase = 0;                 // frameSlot * maxDraws
		                                                         // Cached handle for the current frame's indirect buffer; resolved in
		// PrepareAndDispatch (when frameSlot is known) and consumed in
		// FlushDrawImpl where the per-frame slot is no longer in scope.
		gpu::BufferHandle m_cachedIndirectHandle{};
		bool m_debugForceVisible = false;
		bool m_debugBypassIndirect = false;
		bool m_debugDisableAnimation = false;
		std::uint32_t m_debugAnimPassMask = 0xFFFFFFFFu; // bit 0=PoseInit, 1=AnimSample, 2=NodeFlatten, 3=SkinCopy
		std::uint32_t m_debugLogSkinJobsFramesLeft = 0;

		using LightingPushFn = std::function<void(gpu::CommandList&, gpu::PipelineLayout)>;

		// Shared implementation for FlushDraw / FlushDrawWithFrameAddr / FlushDrawPush.
		void FlushDrawImpl(gpu::CommandList& cmd,
		        gpu::DescriptorSet bindlessSet,
		        gpu::DescriptorSet lightingSet,
		        gpu::DeviceAddress frameAddr,
		        const GraphicsPipeline* overridePipeline,
		        std::uint32_t cascadeOffset,
		        const char* debugLabel,
		        float r,
		        float g,
		        float b,
		        const LightingPushFn& pushLightingFn = {});

		const RenderQueueSharedPipelines* m_sharedPipelines = nullptr;

		const AnimationDatabase* m_animationDb = nullptr;
		std::uint32_t m_animationSampleJobCount = 0;
		std::array<bool, kFramesInFlight> m_animationSlotCleared{};

		AnimationBlendSystem* m_animationBlendSystem = nullptr;
		AnimationIkSystem* m_animationIkSystem = nullptr;
		AnimationRootMotionSystem* m_rootMotionSystem = nullptr;
		std::uint32_t m_hipsNodeIdx = 0;
	};
} // namespace aether
