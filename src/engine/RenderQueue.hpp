#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <vulkan/vulkan.h>

#include "DrawPushConstants.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	class CommandRecorder;
	class GraphicsPipeline;
	class Mesh;

	// A lightweight typed draw-call submission record.
	// Submitted by game/app code; consumed by the engine during EndFrame.
	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr; // must be indexed; null draws are not supported
		std::uint32_t instanceCount = 1;
		glm::mat4 modelMatrix{ 1.0f };             // per-object world transform
		std::uint32_t materialIndex = 0xFFFFFFFFu; // index into MaterialBuffer; 0xFFFF… = fallback
		VkDeviceAddress skinBufferAddr = 0;        // BDA of joint palette; 0 = not skinned
		glm::vec4 worldBoundingSphere{};           // xyz=world center, w=radius; w<=0 = skip culling
	};

	// Per-frame bucket that collects DrawCommands from app/scene code and flushes them
	// via a GPU compute culling pass + indirect draw.
	//
	// Usage per frame:
	//   1. Submit()  — called by scene/world code to enqueue draws.
	//   2. PrepareAndDispatch()  — CPU writes input buffers, dispatches cull compute, inserts barrier.
	//   3. FlushDraw()  — records DrawIndexedIndirect calls per batch using GPU output.
	//   4. Clear()  — resets the queue for the next frame.
	class RenderQueue
	{
	public:
		static constexpr std::uint32_t kFramesInFlight = 2;

		void Initialize(VkDevice device, VmaAllocator allocator, std::uint32_t maxDraws = 8192, std::uint32_t maxBatches = 1024);
		void Shutdown();

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

		// Phase 1 — Compute pass callback.
		// Sorts commands, writes all CPU-side input buffers, dispatches the culling
		// compute shader, and inserts a compute→indirect pipeline barrier.
		// frameAddr is the BDA of the current FrameConstantsData (used for viewProj frustum cull).
		void PrepareAndDispatch(VkCommandBuffer cmd, VkDeviceAddress frameAddr, VkPipeline computePipeline, VkPipelineLayout computeLayout, std::uint32_t frameIndex);

		// Phase 2 — Graphics pass callback.
		// Records one DrawIndexedIndirect per batch (instanceCount=0 skips culled draws),
		// reading the GPU-written output indirect buffer produced by PrepareAndDispatch.
		// Must be called after PrepareAndDispatch on the same frame's command buffer.
		void FlushDraw(CommandRecorder& recorder, VkDescriptorSet bindlessSet = VK_NULL_HANDLE, VkDescriptorSet lightingSet = VK_NULL_HANDLE);

		void Clear();

		[[nodiscard]] bool IsEmpty() const;

	private:
		std::vector<DrawCommand> m_commands;
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;

		// ── CPU-mapped input buffers (written each frame before compute dispatch) ──
		UniqueBuffer m_instanceDataBuffer; // DrawInstanceData[]  — SSBO + BDA
		UniqueBuffer m_cullInputBuffer;    // CullDrawInput[]     — SSBO + BDA
		UniqueBuffer m_batchDescBuffer;    // CullBatch[]         — SSBO + BDA

		DrawInstanceData* m_instanceDataMapped = nullptr;
		CullDrawInput* m_cullInputMapped = nullptr;
		CullBatch* m_batchDescMapped = nullptr;

		// ── GPU-side output (device-local, written by compute, read as indirect) ──
		UniqueBuffer m_outputIndirectBuffer; // VkDrawIndexedIndirectCommand[] — INDIRECT + BDA

		std::uint32_t m_maxDraws = 0;
		std::uint32_t m_maxBatches = 0;

		// ── Per-batch info for FlushDraw, built during PrepareAndDispatch ────────
		struct BatchRenderInfo
		{
			const GraphicsPipeline* pipeline = nullptr;
			const Mesh* mesh = nullptr;
			std::uint32_t outputStart = 0; // first slot in output indirect buffer
			std::uint32_t drawCount = 0;   // capacity = max surviving draws
		};

		std::vector<BatchRenderInfo> m_batchRenderInfos;

		// Values cached from PrepareAndDispatch — used by FlushDraw.
		VkDeviceAddress m_cachedFrameAddr = 0;
		VkDeviceAddress m_cachedInstanceDataAddr = 0; // BDA of DrawInstanceData[0] for current frame slot
		std::uint32_t m_cachedDrawBase = 0;           // frameSlot * maxDraws
		std::uint32_t m_cachedBatchBase = 0;          // frameSlot * maxBatches
		bool m_debugForceVisible = false;
		bool m_debugBypassIndirect = false;
	};
} // namespace aether
