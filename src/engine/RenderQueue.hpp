#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <vulkan/vulkan.h>

#include "AnimationDatabase.hpp"
#include "GpuContracts.hpp"
#include "Swapchain.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	class CommandRecorder;
	class GraphicsPipeline;
	class Mesh;

	// Per-draw submission payload.
	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr; // must be indexed; null draws are not supported
		std::uint32_t instanceCount = 1;
		glm::mat4 modelMatrix{ 1.0f };             // per-object world transform
		std::uint32_t materialIndex = 0xFFFFFFFFu; // index into MaterialBuffer; 0xFFFF… = fallback
		VkDeviceAddress sourceSkinBufferAddr = 0;  // BDA of source joint palette; 0 = not skinned
		std::int32_t skinIndex = -1;               // skin index in AnimationDatabase
		std::uint32_t skinJointCount = 0;          // number of joints in source palette
		std::uint32_t animClipIndex = 0;           // active clip for GPU sampling
		float animTime = 0.0f;                     // active clip time for GPU sampling
		bool gpuSampleEligible = false;            // true when draw has valid animation metadata
		bool nonHeroGpuBlend = false;              // enable compute-side temporal blend path
		glm::vec4 worldBoundingSphere{};           // xyz=world center, w=radius; w<=0 = skip culling
	};

	// Collects draws, runs cull/animation compute, then emits indirect draws.
	class RenderQueue
	{
	public:
		static constexpr std::uint32_t kFramesInFlight = Swapchain::kMaxFramesInFlight;

		void Initialize(VkDevice device, VmaAllocator allocator, std::uint32_t maxDraws = 8192, std::uint32_t maxBatches = 1024);
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

		void SetDebugForceCpuSkinFallback(bool enabled)
		{
			m_debugForceCpuSkinFallback = enabled;
		}

		[[nodiscard]] bool IsDebugForceCpuSkinFallback() const
		{
			return m_debugForceCpuSkinFallback;
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
		UniqueBuffer m_instanceDataBuffer; // DrawInstanceData[]  — SSBO + BDA
		UniqueBuffer m_cullInputBuffer;    // CullDrawInput[]     — SSBO + BDA
		UniqueBuffer m_batchDescBuffer;    // CullBatch[]         — SSBO + BDA

		DrawInstanceData* m_instanceDataMapped = nullptr;
		CullDrawInput* m_cullInputMapped = nullptr;
		CullBatch* m_batchDescMapped = nullptr;

		// Device-local outputs consumed by draw/compute.
		UniqueBuffer m_outputIndirectBuffer; // VkDrawIndexedIndirectCommand[] — INDIRECT + BDA
		UniqueBuffer m_skinPaletteBuffer;    // glm::mat4[] global skin palette pool (device-local)
		UniqueBuffer m_skinCopyJobBuffer;    // SkinCopyJob[] CPU-mapped per-frame copy/blend jobs

		SkinCopyJob* m_skinCopyJobsMapped = nullptr;
		AnimatorSampleJob* m_animationSampleJobsMapped = nullptr;

		std::uint32_t m_maxDraws = 0;
		std::uint32_t m_maxBatches = 0;
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
		bool m_debugForceCpuSkinFallback = false;

		VkPipeline m_skinCopyPipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_skinCopyPipelineLayout = VK_NULL_HANDLE;

		VkPipeline m_animationSamplePipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_animationSamplePipelineLayout = VK_NULL_HANDLE;

		const AnimationDatabase* m_animationDb = nullptr;
		UniqueBuffer m_animationSampleJobsBuffer; // AnimatorSampleJob[] CPU-mapped
		UniqueBuffer m_sampledPosesBuffer;        // SampledNodePose[] GPU-written

		std::uint32_t m_animationSampleJobCount = 0;
		uint64_t m_tracyAnimationCtx = 0;

		void EnsureSkinCopyPipeline();
		void EnsureAnimationSamplePipeline();
	};
} // namespace aether
