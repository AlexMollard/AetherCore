#pragma once

#include <array>
#include <cstdint>
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

		void Initialize(VkDevice device);
		void Shutdown(VkDevice device);
	};

	// Per-draw submission payload.
	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr; // must be indexed; null draws are not supported
		std::uint32_t instanceCount = 1;
		glm::mat4 modelMatrix{ 1.0f };             // per-object world transform
		std::uint32_t materialIndex = 0xFFFFFFFFu; // index into MaterialBuffer; 0xFFFF… = fallback
		std::int32_t skinIndex = -1;               // skin index in AnimationDatabase; -1 = not skinned
		std::uint32_t skinJointCount = 0;          // number of joints in the skin
		std::uint32_t animClipIndex = 0;           // active clip for GPU sampling
		float animTime = 0.0f;                     // active clip time for GPU sampling
		glm::vec4 worldBoundingSphere{};           // xyz=world center, w=radius; w<=0 = skip culling
	};

	// Collects draws, runs cull/animation compute, then emits indirect draws.
	class RenderQueue
	{
	public:
		static constexpr std::uint32_t kFramesInFlight = Swapchain::kMaxFramesInFlight;
		static constexpr std::uint32_t kDefaultMaxAnimationDraws = 1024u;

		// maxAnimationDraws controls the animation-related pool sizes separately
		// from total draw capacity. Pass 0 to disable all animation/skin buffers
		// (for queues that never process skinned draws, e.g. voxel shadow queues).
		// UINT32_MAX (default) derives a sane cap from total draws using
		// kDefaultMaxAnimationDraws instead of assuming every draw can animate.
		void Initialize(VkDevice device, VmaAllocator allocator, const RenderQueueSharedPipelines& pipelines, std::uint32_t maxDraws = 8192, std::uint32_t maxBatches = 1024, std::uint32_t maxAnimationDraws = UINT32_MAX);
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

		// Write inputs and dispatch animation/cull compute.
		void PrepareAndDispatch(VkCommandBuffer cmd, VkDeviceAddress frameAddr, VkPipeline computePipeline, VkPipelineLayout computeLayout, std::uint32_t frameIndex);

		// Emit graphics draws from indirect output.
		void FlushDraw(CommandRecorder& recorder, VkDescriptorSet bindlessSet = VK_NULL_HANDLE, VkDescriptorSet lightingSet = VK_NULL_HANDLE, const GraphicsPipeline* overridePipeline = nullptr);

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
		UniqueBuffer m_instanceDataBuffer; // DrawInstanceData[]  - SSBO + BDA
		UniqueBuffer m_cullInputBuffer;    // CullDrawInput[]     - SSBO + BDA
		UniqueBuffer m_batchDescBuffer;    // CullBatch[]         - SSBO + BDA

		DrawInstanceData* m_instanceDataMapped = nullptr;
		CullDrawInput* m_cullInputMapped = nullptr;
		CullBatch* m_batchDescMapped = nullptr;

		// Device-local outputs consumed by draw/compute.
		UniqueBuffer m_outputIndirectBuffer; // VkDrawIndexedIndirectCommand[] - INDIRECT + BDA
		UniqueBuffer m_skinPaletteBuffer;    // glm::mat4[] global skin palette pool (device-local)
		UniqueBuffer m_skinCopyJobBuffer;    // SkinCopyJob[] CPU-mapped per-frame copy/blend jobs

		SkinCopyJob* m_skinCopyJobsMapped = nullptr;
		AnimatorSampleJob* m_animationSampleJobsMapped = nullptr;

		std::uint32_t m_maxDraws = 0;
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
		VkDeviceAddress m_cachedFrameAddr = 0;
		VkDeviceAddress m_cachedInstanceDataAddr = 0; // BDA of DrawInstanceData[0] for current frame slot
		VkDeviceAddress m_cachedSkinPaletteAddr = 0;  // BDA of global skin palette mat4[0] for current frame slot
		std::uint32_t m_cachedDrawBase = 0;           // frameSlot * maxDraws
		std::uint32_t m_cachedBatchBase = 0;          // frameSlot * maxBatches
		bool m_debugForceVisible = false;
		bool m_debugBypassIndirect = false;
		std::uint32_t m_debugLogSkinJobsFramesLeft = 0;

		const RenderQueueSharedPipelines* m_sharedPipelines = nullptr;

		const AnimationDatabase* m_animationDb = nullptr;
		UniqueBuffer m_animationSampleJobsBuffer; // AnimatorSampleJob[] CPU-mapped
		UniqueBuffer m_sampledPosesBuffer;        // SampledNodePose[] GPU-written

		std::uint32_t m_animationSampleJobCount = 0;
		std::uint32_t m_animationFrameCount = 0;
		bool m_animationBuffersCleared = false;
		TracyVkCtx m_tracyVkCtx = nullptr;

		GpuTimestampPool* m_timestampPool = nullptr;

		// Per-slot timestamp query indices written this frame, used to correlate results
		// with the correct dispatch when BeginFrame reads them back kFramesInFlight later.
		struct TsSlots
		{
			std::uint32_t animSampleStart = UINT32_MAX;
			std::uint32_t animSampleEnd = UINT32_MAX;
			std::uint32_t skinPaletteStart = UINT32_MAX;
			std::uint32_t skinPaletteEnd = UINT32_MAX;
			std::uint32_t cullStart = UINT32_MAX;
			std::uint32_t cullEnd = UINT32_MAX;
		};

		std::array<TsSlots, kFramesInFlight> m_tsSlots{};
	};
} // namespace aether
