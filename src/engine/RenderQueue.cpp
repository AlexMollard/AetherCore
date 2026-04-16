#include "RenderQueue.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "CommandRecorder.hpp"
#include "DrawPushConstants.hpp"
#include "GraphicsPipeline.hpp"
#include "Logger.hpp"
#include "Mesh.hpp"

namespace aether
{
	// ── Helpers ───────────────────────────────────────────────────────────────

	// (Replaced by UniqueBuffer::CreateMapped / UniqueBuffer::CreateDeviceLocal)

	// ── Lifecycle ─────────────────────────────────────────────────────────────

	void RenderQueue::Initialize(VkDevice device, VmaAllocator allocator, std::uint32_t maxDraws, std::uint32_t maxBatches)
	{
		m_device = device;
		m_allocator = allocator;
		m_maxDraws = maxDraws;
		m_maxBatches = maxBatches;

		constexpr VkBufferUsageFlags kSsboFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		// All CPU-mapped and GPU-side buffers are kFramesInFlight deep so that
		// frame N's CPU writes never race with frame N-1's GPU reads.
		m_instanceDataBuffer = UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxDraws) * sizeof(DrawInstanceData), kSsboFlags);
		m_instanceDataMapped = static_cast<DrawInstanceData*>(m_instanceDataBuffer.GetAllocationInfo().pMappedData);

		m_cullInputBuffer = UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxDraws) * sizeof(CullDrawInput), kSsboFlags);
		m_cullInputMapped = static_cast<CullDrawInput*>(m_cullInputBuffer.GetAllocationInfo().pMappedData);

		m_batchDescBuffer = UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxBatches) * sizeof(CullBatch), kSsboFlags);
		m_batchDescMapped = static_cast<CullBatch*>(m_batchDescBuffer.GetAllocationInfo().pMappedData);

		// Output indirect buffer: device-local, written by compute via BDA, read as indirect args.
		m_outputIndirectBuffer = UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxDraws) * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
	}

	void RenderQueue::Shutdown()
	{
		m_outputIndirectBuffer.Reset();
		m_batchDescBuffer.Reset();
		m_cullInputBuffer.Reset();
		m_instanceDataBuffer.Reset();
		m_instanceDataMapped = nullptr;
		m_cullInputMapped = nullptr;
		m_batchDescMapped = nullptr;
		m_maxDraws = 0;
		m_maxBatches = 0;
		m_allocator = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
	}

	// ── Submit ────────────────────────────────────────────────────────────────

	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commands.push_back(cmd);
	}

	// ── PrepareAndDispatch ────────────────────────────────────────────────────

	void RenderQueue::PrepareAndDispatch(VkCommandBuffer cmd, VkDeviceAddress frameAddr, VkPipeline computePipeline, VkPipelineLayout computeLayout, std::uint32_t frameIndex)
	{
		if (m_commands.empty())
		{
			m_batchRenderInfos.clear();
			return;
		}

		m_cachedFrameAddr = frameAddr;
		m_batchRenderInfos.clear();

		// Compute per-frame ring offsets into the double-buffered arrays so that
		// frame N's CPU writes never alias frame N-1's GPU reads.
		const std::uint32_t frameSlot = frameIndex % kFramesInFlight;
		const std::uint32_t drawBase = frameSlot * m_maxDraws;
		const std::uint32_t batchBase = frameSlot * m_maxBatches;
		m_cachedDrawBase = drawBase;
		m_cachedBatchBase = batchBase;
		m_cachedInstanceDataAddr = m_instanceDataBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(drawBase) * sizeof(DrawInstanceData);

		// Sort by pipeline then mesh to group draws into contiguous batches.
		std::stable_sort(m_commands.begin(),
		        m_commands.end(),
		        [](const DrawCommand& a, const DrawCommand& b)
		        {
			        if (a.pipeline != b.pipeline)
				        return a.pipeline < b.pipeline;
			        return a.mesh < b.mesh;
		        });

		const std::uint32_t totalDraws = static_cast<std::uint32_t>(m_commands.size());
		assert(totalDraws <= m_maxDraws && "RenderQueue: exceeded maxDraws — increase Initialize capacity.");

		// Walk sorted commands, write per-draw instance data + cull input, and record batch boundaries.
		std::uint32_t globalDrawIdx = 0; // monotonically increasing index within this frame's slot
		std::uint32_t batchIdx = 0;

		for (std::size_t i = 0; i < m_commands.size();)
		{
			const GraphicsPipeline* batchPipeline = m_commands[i].pipeline;
			const Mesh* batchMesh = m_commands[i].mesh;

			const std::uint32_t batchOutputStart = globalDrawIdx;

			// Find the end of this batch (contiguous same pipeline+mesh).
			std::size_t batchEnd = i;
			while (batchEnd < m_commands.size() && m_commands[batchEnd].pipeline == batchPipeline && m_commands[batchEnd].mesh == batchMesh)
			{
				++batchEnd;
			}
			const std::uint32_t batchDrawCount = static_cast<std::uint32_t>(batchEnd - i);

			assert(batchIdx < m_maxBatches && "RenderQueue: exceeded maxBatches — increase Initialize capacity.");

			// Write per-draw data for this batch into the current frame's ring slot.
			for (std::size_t j = i; j < batchEnd; ++j)
			{
				const DrawCommand& dc = m_commands[j];

				m_instanceDataMapped[drawBase + globalDrawIdx] = DrawInstanceData{
					.model = dc.modelMatrix,
					.materialIndex = dc.materialIndex,
					.skinBufferAddr = dc.skinBufferAddr,
					.worldBoundingSphere = dc.worldBoundingSphere,
				};

				m_cullInputMapped[drawBase + globalDrawIdx] = CullDrawInput{
					.indexCount = (dc.mesh != nullptr) ? dc.mesh->GetIndexCount() : 0u,
					.instanceCount = 1u,
					.firstIndex = 0u,
					.vertexOffset = 0,
					.firstInstance = globalDrawIdx, // frame-relative; BDA base (m_cachedInstanceDataAddr) accounts for slot
					.batchIndex = batchIdx,
				};

				++globalDrawIdx;
			}

			// Write batch descriptor into the current frame's ring slot.
			m_batchDescMapped[batchBase + batchIdx] = CullBatch{ batchOutputStart, batchDrawCount, batchOutputStart };

			m_batchRenderInfos.push_back(BatchRenderInfo{
			        .pipeline = batchPipeline,
			        .mesh = batchMesh,
			        .outputStart = batchOutputStart,
			        .drawCount = batchDrawCount,
			});

			++batchIdx;
			i = batchEnd;
		}

		// Explicitly flush host writes for non-coherent mapped memory before GPU reads.
		// Flush full allocations to avoid nonCoherentAtomSize alignment edge cases on subranges.
		vmaFlushAllocation(m_allocator, m_instanceDataBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
		vmaFlushAllocation(m_allocator, m_cullInputBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
		vmaFlushAllocation(m_allocator, m_batchDescBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);

		// HOST → SHADERS barrier: ensures all CPU buffer writes above are visible to
		// both compute (cull input/desc) and graphics shaders (instance data).
		const VkMemoryBarrier2 hostToShaders{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
			.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		};
		const VkDependencyInfo hostToShaderDep{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &hostToShaders,
		};
		vkCmdPipelineBarrier2(cmd, &hostToShaderDep);

		// All BDAs are offset to this frame's ring slot so the compute shader
		// addresses frame-relative indices starting at 0.
		const VkDeviceSize inputCmdOffset = static_cast<VkDeviceSize>(drawBase) * sizeof(CullDrawInput);
		const VkDeviceSize outputCmdOffset = static_cast<VkDeviceSize>(drawBase) * sizeof(VkDrawIndexedIndirectCommand);
		const VkDeviceSize batchDescOffset = static_cast<VkDeviceSize>(batchBase) * sizeof(CullBatch);
		const CullPushConstants pc{
			.frameAddr = frameAddr,
			.instanceDataAddr = m_cachedInstanceDataAddr,
			.inputCmdAddr = m_cullInputBuffer.GetDeviceAddress() + inputCmdOffset,
			.outputCmdAddr = m_outputIndirectBuffer.GetDeviceAddress() + outputCmdOffset,
			.batchDescAddr = m_batchDescBuffer.GetDeviceAddress() + batchDescOffset,
			.batchCountAddr = 0, // reserved
			.totalDrawCount = totalDraws,
			.debugFlags = m_debugForceVisible ? kCullDebugForceVisibleBit : 0u,
		};

		CommandRecorder(cmd).BeginDebugLabel("CullPass.cullDraws", 0.4f, 0.8f, 0.4f, 1.0f);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
		vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		const std::uint32_t groups = (totalDraws + 63u) / 64u;
		vkCmdDispatch(cmd, groups, 1, 1);
		CommandRecorder(cmd).EndDebugLabel();

		// COMPUTE → DRAW_INDIRECT barrier: output indirect buffer must be
		// fully written before the GPU reads it as indirect draw arguments.
		const VkMemoryBarrier2 computeToIndirect{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
			.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
		};
		const VkDependencyInfo computeToIndirectDep{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &computeToIndirect,
		};
		vkCmdPipelineBarrier2(cmd, &computeToIndirectDep);
	}

	// ── FlushDraw ─────────────────────────────────────────────────────────────

	void RenderQueue::FlushDraw(CommandRecorder& recorder, VkDescriptorSet bindlessSet, VkDescriptorSet lightingSet)
	{
		if (!recorder.IsValid())
			return;
		if (m_batchRenderInfos.empty())
			return;

		recorder.BeginDebugLabel("RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f, 1.0f);

		const DrawPushConstants sharedPc{
			.frameAddr = m_cachedFrameAddr,
			.instanceDataAddr = m_cachedInstanceDataAddr,
		};

		const GraphicsPipeline* lastPipeline = nullptr;
		const Mesh* lastMesh = nullptr;
		VkBuffer lastVertexBuffer = VK_NULL_HANDLE;
		VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
		const GraphicsPipeline* lastSetPipeline = nullptr;
		const GraphicsPipeline* lastLightingSetPipeline = nullptr;

		for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(m_batchRenderInfos.size()); ++bi)
		{
			const BatchRenderInfo& batch = m_batchRenderInfos[bi];

			if (batch.pipeline != nullptr && batch.pipeline != lastPipeline)
			{
				recorder.BindGraphicsPipeline(*batch.pipeline);
				lastPipeline = batch.pipeline;
				lastSetPipeline = nullptr;
				lastLightingSetPipeline = nullptr;
			}

			if (bindlessSet != VK_NULL_HANDLE && batch.pipeline != nullptr && batch.pipeline != lastSetPipeline)
			{
				vkCmdBindDescriptorSets(recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, batch.pipeline->GetLayout(), 0, 1, &bindlessSet, 0, nullptr);
				lastSetPipeline = batch.pipeline;
			}

			if (lightingSet != VK_NULL_HANDLE && batch.pipeline != nullptr && batch.pipeline->GetSetLayoutCount() > 1 && batch.pipeline != lastLightingSetPipeline)
			{
				vkCmdBindDescriptorSets(recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, batch.pipeline->GetLayout(), 1, 1, &lightingSet, 0, nullptr);
				lastLightingSetPipeline = batch.pipeline;
			}

			if (batch.pipeline != nullptr)
				recorder.PushConstants(batch.pipeline->GetLayout(), sharedPc);

			if (batch.mesh != nullptr)
			{
				if (batch.mesh->GetBuffer() != lastVertexBuffer)
				{
					recorder.BindVertexBuffer(batch.mesh->GetBuffer());
					lastVertexBuffer = batch.mesh->GetBuffer();
				}
				if (batch.mesh->GetIndexBuffer() != lastIndexBuffer)
				{
					recorder.BindIndexBuffer(batch.mesh->GetIndexBuffer());
					lastIndexBuffer = batch.mesh->GetIndexBuffer();
				}
				lastMesh = batch.mesh;
			}

			if (m_debugBypassIndirect)
			{
				const std::uint32_t indexCount = (batch.mesh != nullptr) ? batch.mesh->GetIndexCount() : 0u;
				for (std::uint32_t local = 0; local < batch.drawCount; ++local)
				{
					recorder.DrawIndexed(indexCount, 1u, 0u, 0, batch.outputStart + local);
				}
			}
			else
			{
				recorder.DrawIndexedIndirect(m_outputIndirectBuffer.Get(), static_cast<VkDeviceSize>(m_cachedDrawBase + batch.outputStart) * sizeof(VkDrawIndexedIndirectCommand), batch.drawCount, sizeof(VkDrawIndexedIndirectCommand));
			}
		}

		recorder.EndDebugLabel();
	}

	// ── Clear / query ─────────────────────────────────────────────────────────

	void RenderQueue::Clear()
	{
		m_commands.clear();
		m_batchRenderInfos.clear();
	}

	bool RenderQueue::IsEmpty() const
	{
		return m_commands.empty();
	}
} // namespace aether
