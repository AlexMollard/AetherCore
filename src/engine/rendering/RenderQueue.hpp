#pragma once

#include <functional>
#include <array>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <vector>
#include "vulkan/volk.hpp"

#include "utils/GpuProfiler.hpp"
#include "animation/AnimationDatabase.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/GpuTimestampPool.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/UniqueBuffer.hpp"

namespace aether
{
	class AnimationBlendSystem;
	class AnimationIkSystem;
	class AnimationRootMotionSystem;
	class CommandRecorder;
	class GraphicsPipeline;
	class Mesh;

	// Shared compute pipelines used by every RenderQueue instance.
	// Create one, initialize it once, then pass a const reference to each RenderQueue::Initialize().
	struct RenderQueueSharedPipelines
	{
		VkPipeline skinCopy = VK_NULL_HANDLE;
		VkPipelineLayout skinCopyLayout = VK_NULL_HANDLE;
		VkPipeline animSample = VK_NULL_HANDLE;
		VkPipelineLayout animSampleLayout = VK_NULL_HANDLE;
		VkPipeline nodeFlatten = VK_NULL_HANDLE;
		VkPipelineLayout nodeFlattenLayout = VK_NULL_HANDLE;
		VkPipeline poseInit = VK_NULL_HANDLE;
		VkPipelineLayout poseInitLayout = VK_NULL_HANDLE;
		VkPipeline animBlend = VK_NULL_HANDLE;
		VkPipelineLayout animBlendLayout = VK_NULL_HANDLE;
		VkPipeline ikSolve = VK_NULL_HANDLE;
		VkPipelineLayout ikSolveLayout = VK_NULL_HANDLE;

		void Initialize(VkDevice device, VkPipelineCache pipelineCache);
		void Shutdown(VkDevice device);
	};

	// Per-draw submission payload.
	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr; // must be indexed; null draws are not supported
		std::uint32_t instanceCount = 1;
		glm::mat4 modelMatrix{1.0f};               // per-object world transform
		std::uint32_t materialIndex = 0xFFFFFFFFu; // index into MaterialBuffer; 0xFFFF… = fallback
		std::int32_t skinIndex = -1;               // skin index in AnimationDatabase; -1 = not skinned
		std::uint32_t skinJointCount = 0;          // number of joints in the skin
		std::uint32_t animClipIndex = 0;           // active clip for GPU sampling
		float animTime = 0.0f;                     // active clip time for GPU sampling
		glm::vec4 worldBoundingSphere{};           // xyz=world center, w=radius; w<=0 = skip culling
		const AnimationDatabase* animDb = nullptr; // per-draw animation database for GPU sampling
	};

	// Collects draws, runs cull/animation compute, then emits indirect draws.
	class RenderQueue
	{
	public:
		static constexpr std::uint32_t kFramesInFlight = Swapchain::kMaxFramesInFlight;
		static constexpr std::uint32_t kDefaultMaxAnimationDraws = 1024u;

		// Configuration struct for Initialize - replaces multiple parameters.
		// maxAnimationDraws controls animation pool sizes separately from total draw capacity.
		// Pass 0 to disable all animation/skin buffers (for non-skinned queues).
		// UINT32_MAX (default) derives a sane cap from total draws using kDefaultMaxAnimationDraws.
		// outputDrawCapacity overrides per-frame indirect output buffer capacity (0 = equals maxDraws).
		// Use e.g. maxDraws * 3 for multi-frustum shadow queues writing 3 cascade regions.
		struct Config
		{
			std::uint32_t maxDraws = 8192;
			std::uint32_t maxBatches = 1024;
			std::uint32_t maxAnimationDraws = UINT32_MAX;
			std::uint32_t outputDrawCapacity = 0;
		};

		// Initialize with configuration struct.
		void Initialize(VkDevice device, VmaAllocator allocator, const RenderQueueSharedPipelines& pipelines, const Config& config = {});
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

		// Optional GPU timestamp pool for measuring dispatch durations.
		// When set, BeginFrame/Write/Readback are called automatically and results
		// are emitted as TracyPlot entries: GPU/AnimSample_ms, GPU/SkinPalette_ms, GPU/Cull_ms.
		void SetTimestampPool(GpuTimestampPool* pool)
		{
			m_timestampPool = pool;
		}

		// Optional Tracy GPU context for CPU-correlated GPU timeline zones.
		// Pass VulkanContext::GetTracyVkCtx() after initialization.
		void SetTracyVkCtx(TracyVkCtx ctx)
		{
			m_tracyVkCtx = ctx;
		}

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

		// Hips node index for root motion copy: vkCmdCopyBuffer reads from
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
		void PrepareAndDispatch(VkCommandBuffer cmd, VkDeviceAddress frameAddr, VkPipeline computePipeline, VkPipelineLayout computeLayout, std::uint32_t frameIndex);

		// For multi-frustum queues: provides the 3 cascade frame constant BDAs
		// used by PrepareAndDispatch to build CullMultiPushConstants.
		void SetMultiCullFrameAddrs(const VkDeviceAddress addrs[3])
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

		[[nodiscard]] VkDeviceAddress GetSkinPaletteBufferAddress() const
		{
			return m_skinPaletteBuffer.GetDeviceAddress();
		}

		// Emit graphics draws from indirect output.
		// cascadeOffset is added to the output buffer offset (in VkDrawIndexedIndirectCommand units);
		// used by multi-frustum queues to select one cascade's output region.
		void FlushDraw(CommandRecorder& recorder, VkDescriptorSet bindlessSet = VK_NULL_HANDLE, VkDescriptorSet lightingSet = VK_NULL_HANDLE, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0);
		void FlushDrawPush(CommandRecorder& recorder, VkDescriptorSet bindlessSet, std::function<void(VkCommandBuffer, VkPipelineLayout)> pushLightingFn, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0);

		// Same as FlushDraw but overrides the frame constants BDA in push constants
		// with overrideFrameAddr. Used for rendering the same geometry from multiple POVs
		// (e.g., local shadow atlas where each light has a different VP matrix).
		void FlushDrawWithFrameAddr(CommandRecorder& recorder, VkDescriptorSet bindlessSet, VkDescriptorSet lightingSet, VkDeviceAddress overrideFrameAddr, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0);

		// Clear queued commands for a frame slot.
		void Clear(std::uint32_t slot);

		[[nodiscard]] bool IsEmpty(std::uint32_t slot) const;

	private:
		// Per-frame queued draw commands.
		std::array<std::vector<DrawCommand>, kFramesInFlight> m_commandSlots;
		std::uint32_t m_writeSlot = 0; // set by game thread via SetWriteSlot()
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;

		// CPU-written per-frame inputs.
		UniqueBuffer m_instanceDataBuffer; // DrawContracts::InstanceData[]  - SSBO + BDA
		UniqueBuffer m_cullInputBuffer;    // CullContracts::DrawInput[]     - SSBO + BDA
		UniqueBuffer m_batchDescBuffer;    // CullContracts::Batch[]         - SSBO + BDA

		DrawContracts::InstanceData* m_instanceDataMapped = nullptr;
		CullContracts::DrawInput* m_cullInputMapped = nullptr;
		CullContracts::Batch* m_batchDescMapped = nullptr;

		// Device-local outputs consumed by draw/compute.
		UniqueBuffer m_outputIndirectBuffer;       // VkDrawIndexedIndirectCommand[] - INDIRECT + BDA
		UniqueBuffer m_skinPaletteBuffer;          // glm::mat4[] global skin palette pool (device-local)
		UniqueBuffer m_skinCopyJobBuffer;          // AnimationContracts::SkinCopyJob[] CPU-mapped per-frame copy/blend jobs
		UniqueBuffer m_nodeGlobalTransformsBuffer; // float4x4[] per-node global transforms (flatten pass output)

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
			std::uint32_t outputStart = 0; // first slot in output indirect buffer
			std::uint32_t drawCount = 0;   // capacity = max surviving draws
		};

		std::vector<BatchRenderInfo> m_batchRenderInfos;

		// Cached per-frame addresses/state for FlushDraw.
		VkDeviceAddress m_multiFrameAddrs[3] = {};
		VkDeviceAddress m_cachedFrameAddr = 0;
		VkDeviceAddress m_cachedInstanceDataAddr = 0;         // BDA of DrawContracts::InstanceData[0] for current frame slot
		VkDeviceAddress m_cachedSkinPaletteAddr = 0;          // BDA of global skin palette mat4[0] for current frame slot
		VkDeviceAddress m_cachedNodeGlobalTransformsAddr = 0; // BDA of per-node global transforms for current frame slot
		VkDeviceAddress m_cachedDrawBase = 0;                 // frameSlot * maxDraws
		VkDeviceAddress m_cachedBatchBase = 0;                // frameSlot * maxBatches
		bool m_debugForceVisible = false;
		bool m_debugBypassIndirect = false;
		bool m_debugDisableAnimation = false;
		std::uint32_t m_debugAnimPassMask = 0xFFFFFFFFu; // bit 0=PoseInit, 1=AnimSample, 2=NodeFlatten, 3=SkinCopy
		std::uint32_t m_debugLogSkinJobsFramesLeft = 0;

		using LightingPushFn = std::function<void(VkCommandBuffer, VkPipelineLayout)>;

		// Shared implementation for FlushDraw / FlushDrawWithFrameAddr / FlushDrawPush.
		void FlushDrawImpl(CommandRecorder& recorder,
		        VkDescriptorSet bindlessSet,
		        VkDescriptorSet lightingSet,
		        VkDeviceAddress frameAddr,
		        const GraphicsPipeline* overridePipeline,
		        std::uint32_t cascadeOffset,
		        const char* debugLabel,
		        float r,
		        float g,
		        float b,
		        const LightingPushFn& pushLightingFn = {});

		const RenderQueueSharedPipelines* m_sharedPipelines = nullptr;

		const AnimationDatabase* m_animationDb = nullptr;
		UniqueBuffer m_animationSampleJobsBuffer; // AnimationContracts::AnimatorSampleJob[] CPU-mapped
		UniqueBuffer m_sampledPosesBuffer;        // AnimationContracts::SampledNodePose[] GPU-written
		std::uint32_t m_animationSampleJobCount = 0;
		std::uint32_t m_animationFrameCount = 0;
		std::array<bool, kFramesInFlight> m_animationSlotCleared{};
		TracyVkCtx m_tracyVkCtx = nullptr;

		AnimationBlendSystem* m_animationBlendSystem = nullptr;
		AnimationIkSystem* m_animationIkSystem = nullptr;
		AnimationRootMotionSystem* m_rootMotionSystem = nullptr;
		std::uint32_t m_hipsNodeIdx = 0;

		GpuTimestampPool* m_timestampPool = nullptr;

		// Per-slot timestamp query indices written this frame, used to correlate results
		// with the correct dispatch when BeginFrame reads them back kFramesInFlight later.
		struct TsSlots
		{
			std::uint32_t animSampleStart = UINT32_MAX;
			std::uint32_t animSampleEnd = UINT32_MAX;
			std::uint32_t nodeFlattenStart = UINT32_MAX;
			std::uint32_t nodeFlattenEnd = UINT32_MAX;
			std::uint32_t skinPaletteStart = UINT32_MAX;
			std::uint32_t skinPaletteEnd = UINT32_MAX;
			std::uint32_t cullStart = UINT32_MAX;
			std::uint32_t cullEnd = UINT32_MAX;
		};

		std::array<TsSlots, kFramesInFlight> m_tsSlots{};
	};
} // namespace aether
