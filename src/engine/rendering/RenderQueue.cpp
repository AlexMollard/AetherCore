#include "rendering/RenderQueue.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "animation/AnimationBlend.hpp"
#include "animation/AnimationIk.hpp"
#include "rendering/CommandRecorder.hpp"
#include "io/FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "mesh/Mesh.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	void RenderQueue::Initialize(VkDevice device, VmaAllocator allocator, const RenderQueueSharedPipelines& pipelines, const RenderQueueConfig& config)
	{
		m_device = device;
		m_allocator = allocator;
		m_sharedPipelines = &pipelines;
		m_maxDraws = config.maxDraws;
		m_maxBatches = config.maxBatches;
		m_outputDrawCapacity = (config.outputDrawCapacity > 0) ? config.outputDrawCapacity : config.maxDraws;
		m_maxAnimationDraws = (config.maxAnimationDraws == UINT32_MAX) ? std::min(config.maxDraws, kDefaultMaxAnimationDraws) : config.maxAnimationDraws;
		m_maxSkinJoints = m_maxAnimationDraws * 128u;
		m_maxSampledPoses = m_maxSkinJoints * 2u;
		AE_INFO(LogCategory::Render, "RenderQueue::Initialize: maxDraws={}, maxAnimationDraws={}, maxSkinJoints={}, maxSampledPoses={}", m_maxDraws, m_maxAnimationDraws, m_maxSkinJoints, m_maxSampledPoses);

		constexpr VkBufferUsageFlags kSsboFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		AE_EXPECT_OR_THROW(b0, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(config.maxDraws) * sizeof(DrawContracts::InstanceData), kSsboFlags, "RenderQueue.InstanceData"));
		m_instanceDataBuffer = std::move(b0);
		m_instanceDataMapped = static_cast<DrawContracts::InstanceData*>(m_instanceDataBuffer.GetAllocationInfo().pMappedData);

		AE_EXPECT_OR_THROW(b1, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(config.maxDraws) * sizeof(CullContracts::DrawInput), kSsboFlags, "RenderQueue.CullInput"));
		m_cullInputBuffer = std::move(b1);
		m_cullInputMapped = static_cast<CullContracts::DrawInput*>(m_cullInputBuffer.GetAllocationInfo().pMappedData);

		AE_EXPECT_OR_THROW(b2, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(config.maxBatches) * sizeof(CullContracts::Batch), kSsboFlags, "RenderQueue.BatchDesc"));
		m_batchDescBuffer = std::move(b2);
		m_batchDescMapped = static_cast<CullContracts::Batch*>(m_batchDescBuffer.GetAllocationInfo().pMappedData);

		if (m_maxAnimationDraws > 0u)
		{
			AE_EXPECT_OR_THROW(b3, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxAnimationDraws) * sizeof(AnimationContracts::SkinCopyJob), kSsboFlags, "RenderQueue.SkinCopyJobs"));
			m_skinCopyJobBuffer = std::move(b3);
			m_skinCopyJobsMapped = static_cast<AnimationContracts::SkinCopyJob*>(m_skinCopyJobBuffer.GetAllocationInfo().pMappedData);

			AE_EXPECT_OR_THROW(b4, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxAnimationDraws) * sizeof(AnimationContracts::AnimatorSampleJob), kSsboFlags, "RenderQueue.AnimSampleJobs"));
			m_animationSampleJobsBuffer = std::move(b4);
			m_animationSampleJobsMapped = static_cast<AnimationContracts::AnimatorSampleJob*>(m_animationSampleJobsBuffer.GetAllocationInfo().pMappedData);

			constexpr VkBufferUsageFlags kAnimationSsboFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			AE_EXPECT_OR_THROW(b5, UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4), kAnimationSsboFlags, "RenderQueue.SkinPalette"));
			m_skinPaletteBuffer = std::move(b5);

			AE_EXPECT_OR_THROW(b6, UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(AnimationContracts::SampledNodePose), kAnimationSsboFlags, "RenderQueue.SampledPoses"));
			m_sampledPosesBuffer = std::move(b6);

			AE_EXPECT_OR_THROW(b7, UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(glm::mat4), kAnimationSsboFlags, "RenderQueue.NodeGlobalTransforms"));
			m_nodeGlobalTransformsBuffer = std::move(b7);
			AE_INFO(LogCategory::Render,
			        "RenderQueue animation buffers: skinPalette=0x{:x}, sampledPoses=0x{:x}, nodeGlobalTransforms=0x{:x}",
			        m_skinPaletteBuffer.GetDeviceAddress(),
			        m_sampledPosesBuffer.GetDeviceAddress(),
			        m_nodeGlobalTransformsBuffer.GetDeviceAddress());
		}

		AE_EXPECT_OR_THROW(b8,
		        UniqueBuffer::CreateDeviceLocal(allocator,
		                device,
		                kFramesInFlight * static_cast<VkDeviceSize>(m_outputDrawCapacity) * sizeof(VkDrawIndexedIndirectCommand),
		                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		                "RenderQueue.IndirectOutput"));
		m_outputIndirectBuffer = std::move(b8);
	}

	void RenderQueue::Shutdown()
	{
		m_sharedPipelines = nullptr;
		m_sampledPosesBuffer.Reset();
		m_animationSampleJobsBuffer.Reset();
		m_outputIndirectBuffer.Reset();
		m_skinPaletteBuffer.Reset();
		m_skinCopyJobBuffer.Reset();
		m_nodeGlobalTransformsBuffer.Reset();
		m_batchDescBuffer.Reset();
		m_cullInputBuffer.Reset();
		m_instanceDataBuffer.Reset();
		m_animationSampleJobsMapped = nullptr;
		m_skinCopyJobsMapped = nullptr;
		m_instanceDataMapped = nullptr;
		m_cullInputMapped = nullptr;
		m_batchDescMapped = nullptr;
		m_maxDraws = 0;
		m_outputDrawCapacity = 0;
		m_maxBatches = 0;
		m_maxAnimationDraws = 0;
		m_maxSkinJoints = 0;
		m_maxSampledPoses = 0;
		m_animationSlotCleared = {};
		m_allocator = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
	}

	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commandSlots[m_writeSlot].push_back(cmd);
	}

	void RenderQueue::PrepareAndDispatch(gpu::CommandList& cmdList, gpu::DeviceAddress frameAddr, VkPipeline computePipeline, VkPipelineLayout computeLayout, std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		// TODO(phase5): migrate these raw vkCmd* sites to gpu::CommandList methods.
		// Transitional shim: the body of this function still uses raw vkCmd*
		// calls for compute-pipeline bind/dispatch/barrier; unwrap the opaque
		// command-buffer handle once and reuse it locally.
		VkCommandBuffer cmd = static_cast<VkCommandBuffer>(cmdList.GetCommandBuffer());
		std::vector<DrawCommand>& m_commands = m_commandSlots[frameIndex % kFramesInFlight];
		if (m_commands.empty())
		{
			m_batchRenderInfos.clear();
			return;
		}

		m_cachedFrameAddr = frameAddr;
		m_batchRenderInfos.clear();
		m_animationSampleJobCount = 0;

		const std::uint32_t frameSlot = frameIndex % kFramesInFlight;
		const std::uint32_t drawBase = frameSlot * m_maxDraws;
		const std::uint32_t batchBase = frameSlot * m_maxBatches;
		const std::uint32_t animJobBase = frameSlot * m_maxAnimationDraws;

		// GPU timestamp readback: read results from kFramesInFlight frames ago, reset slot for this frame.
		if (m_timestampPool && m_timestampPool->IsValid())
		{
			float tsMs[GpuTimestampPool::kMaxTimestamps]{};
			const std::uint32_t readCount = m_timestampPool->BeginFrame(frameIndex, tsMs);
			const TsSlots& prev = m_tsSlots[frameSlot];
#ifdef TRACY_ENABLE
			if (prev.animSampleStart != UINT32_MAX && prev.animSampleEnd < readCount)
			{
				TracyPlot("GPU/AnimSample_ms", static_cast<double>(tsMs[prev.animSampleEnd] - tsMs[prev.animSampleStart]));
			}
			if (prev.nodeFlattenStart != UINT32_MAX && prev.nodeFlattenEnd < readCount)
			{
				TracyPlot("GPU/NodeFlatten_ms", static_cast<double>(tsMs[prev.nodeFlattenEnd] - tsMs[prev.nodeFlattenStart]));
			}
			if (prev.skinPaletteStart != UINT32_MAX && prev.skinPaletteEnd < readCount)
			{
				TracyPlot("GPU/SkinPalette_ms", static_cast<double>(tsMs[prev.skinPaletteEnd] - tsMs[prev.skinPaletteStart]));
			}
			if (prev.cullStart != UINT32_MAX && prev.cullEnd < readCount)
			{
				TracyPlot("GPU/Cull_ms", static_cast<double>(tsMs[prev.cullEnd] - tsMs[prev.cullStart]));
			}
#endif
			m_tsSlots[frameSlot] = {};
		}

		// Clear device-local animation buffers on first use of each frame slot.
		// Each slot is cleared individually so in-flight slots don't sit with garbage
		// until the first animation sample writes their data.
		if (!m_animationSlotCleared[frameSlot])
		{
			m_animationSlotCleared[frameSlot] = true;
			if (m_skinPaletteBuffer)
			{
				const VkDeviceSize slotSize = static_cast<VkDeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4);
				vkCmdFillBuffer(cmd, m_skinPaletteBuffer.Get(), static_cast<VkDeviceSize>(frameSlot) * slotSize, slotSize, 0);
			}
			if (m_sampledPosesBuffer)
			{
				const VkDeviceSize slotSize = static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(AnimationContracts::SampledNodePose);
				vkCmdFillBuffer(cmd, m_sampledPosesBuffer.Get(), static_cast<VkDeviceSize>(frameSlot) * slotSize, slotSize, 0);
			}
			if (m_nodeGlobalTransformsBuffer)
			{
				const VkDeviceSize slotSize = static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(glm::mat4);
				vkCmdFillBuffer(cmd, m_nodeGlobalTransformsBuffer.Get(), static_cast<VkDeviceSize>(frameSlot) * slotSize, slotSize, 0);
			}
			const VkMemoryBarrier2 fillToCompute{
			        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			};
			const VkDependencyInfo fillToComputeDep{
			        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			        .memoryBarrierCount = 1,
			        .pMemoryBarriers = &fillToCompute,
			};
			vkCmdPipelineBarrier2(cmd, &fillToComputeDep);
		}
		m_cachedDrawBase = frameSlot * m_outputDrawCapacity;
		m_cachedBatchBase = batchBase;
		m_cachedInstanceDataAddr = m_instanceDataBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(drawBase) * sizeof(DrawContracts::InstanceData);
		const gpu::DeviceAddress currSkinPaletteAddr = (m_maxSkinJoints > 0u) ? m_skinPaletteBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(frameSlot) * static_cast<VkDeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4) : 0;
		const gpu::DeviceAddress currSampledPosesAddr =
		        (m_maxSampledPoses > 0u) ? m_sampledPosesBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(frameSlot) * static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(AnimationContracts::SampledNodePose) : 0;
		const gpu::DeviceAddress currNodeGlobalTransformsAddr = (m_maxSampledPoses > 0u) ? m_nodeGlobalTransformsBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(frameSlot) * static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(glm::mat4) : 0;
		m_cachedNodeGlobalTransformsAddr = currNodeGlobalTransformsAddr;
		m_cachedSkinPaletteAddr = currSkinPaletteAddr;
		const bool gpuSamplingEnabled = m_animationSampleJobsMapped != nullptr && m_skinCopyJobsMapped != nullptr && m_maxAnimationDraws > 0u;

		std::uint32_t skinJointCursor = 0;
		std::uint32_t nodePoseCursor = 0;
		std::uint32_t skinJobCount = 0;
		std::uint32_t sampleJobsThisFrame = 0;

		struct AnimSampleBatch
		{
			const AnimationDatabase* db;
			std::uint32_t dbGeneration;
			std::uint32_t startJob;
			std::uint32_t count;
		};

		struct SkinPaletteBatch
		{
			const AnimationDatabase* db;
			std::uint32_t dbGeneration;
			std::uint32_t startJob;
			std::uint32_t count;
		};

		AnimSampleBatch animSampleBatches[64];
		std::uint32_t animSampleBatchCount = 0;
		SkinPaletteBatch skinPaletteBatches[64];
		std::uint32_t skinPaletteBatchCount = 0;

		std::stable_sort(m_commands.begin(),
		        m_commands.end(),
		        [](const DrawCommand& a, const DrawCommand& b)
		        {
			        if (a.pipeline != b.pipeline)
			        {
				        return a.pipeline < b.pipeline;
			        }
			        return a.mesh < b.mesh;
		        });

		const std::uint32_t totalDraws = static_cast<std::uint32_t>(m_commands.size());
		AE_ASSERT_ALWAYS(totalDraws <= m_maxDraws, "RenderQueue: exceeded maxDraws - increase Initialize capacity.");

		std::uint32_t globalDrawIdx = 0; // monotonically increasing index within this frame slot
		std::uint32_t batchIdx = 0;

		for (std::size_t i = 0; i < m_commands.size();)
		{
			const GraphicsPipeline* batchPipeline = m_commands[i].pipeline;
			const Mesh* batchMesh = m_commands[i].mesh;

			std::size_t batchEnd = i;
			while (batchEnd < m_commands.size() && m_commands[batchEnd].pipeline == batchPipeline && m_commands[batchEnd].mesh == batchMesh)
			{
				++batchEnd;
			}

			if (batchMesh == nullptr || !batchMesh->IsAlive() || !batchMesh->IsValid() || batchMesh->GetIndexBuffer() == VK_NULL_HANDLE)
			{
				i = batchEnd;
				continue;
			}

			const std::uint32_t batchOutputStart = globalDrawIdx;
			const std::uint32_t batchDrawCount = static_cast<std::uint32_t>(batchEnd - i);

			AE_ASSERT_ALWAYS(batchIdx < m_maxBatches, "RenderQueue: exceeded maxBatches - increase Initialize capacity.");

			for (std::size_t j = i; j < batchEnd; ++j)
			{
				const DrawCommand& dc = m_commands[j];

				if (dc.mesh && (!dc.mesh->IsAlive() || !dc.mesh->IsValid() || dc.mesh->GetGeneration() != dc.meshGeneration || dc.mesh->GetIndexBuffer() == VK_NULL_HANDLE))
				{
					continue;
				}

				std::uint32_t skinPaletteOffset = 0;
				std::uint32_t skinJointCount = 0;
				const AnimationDatabase* drawAnimDb = dc.animDb;
				const bool generationValid = drawAnimDb != nullptr && (dc.animDbGeneration == 0 || (drawAnimDb->IsAlive() && dc.animDbGeneration == drawAnimDb->GetGeneration()));
				const bool dbValid = drawAnimDb != nullptr && drawAnimDb->IsValid() && generationValid;
				const std::uint32_t drawClipCount = dbValid ? drawAnimDb->GetClipCount() : 0u;
				const std::uint32_t drawNodeCount = dbValid ? drawAnimDb->GetNodeCount() : 0u;
				const std::uint32_t drawSkinCount = dbValid ? drawAnimDb->GetSkinCount() : 0u;

				const bool wantsGpuSampling = gpuSamplingEnabled && dbValid && dc.skinJointCount > 0 && dc.skinIndex >= 0 && dc.animClipIndex < drawClipCount && static_cast<std::uint32_t>(dc.skinIndex) < drawSkinCount;
				if (wantsGpuSampling)
				{
					if (skinJointCursor + dc.skinJointCount > m_maxSkinJoints)
					{
						AE_WARN(LogCategory::Animation, "RenderQueue: sampled skin palette pool overflow (needed {}, cap {}) - dropping GPU skinning for this draw.", skinJointCursor + dc.skinJointCount, m_maxSkinJoints);
					}
					else if (drawNodeCount == 0u || nodePoseCursor + drawNodeCount > m_maxSampledPoses)
					{
						AE_WARN(LogCategory::Animation, "RenderQueue: sampled node-pose pool overflow (needed {}, cap {}) - dropping GPU skinning for this draw.", nodePoseCursor + drawNodeCount, m_maxSampledPoses);
					}
					else if (skinJobCount >= m_maxAnimationDraws || sampleJobsThisFrame >= m_maxAnimationDraws)
					{
						AE_WARN(LogCategory::Animation, "RenderQueue: animation job overflow (jobs {}, cap {}) - dropping GPU skinning for this draw.", std::max(skinJobCount, sampleJobsThisFrame), m_maxAnimationDraws);
					}
					else
					{
						skinPaletteOffset = skinJointCursor;
						skinJointCount = dc.skinJointCount;

						AnimationContracts::AnimatorSampleJob animJob{};
						animJob.animClipIndex = dc.animClipIndex;
						animJob.animTime = dc.animTime;
						animJob.nodePoseOffset = nodePoseCursor;
						animJob.nodeCount = drawNodeCount;
						animJob.clipsAddr = drawAnimDb->GetClipsAddr();
						animJob.channelsAddr = drawAnimDb->GetChannelsAddr();
						animJob.timesAddr = drawAnimDb->GetTimesAddr();
						animJob.valuesAddr = drawAnimDb->GetValuesAddr();
						animJob.clipCount = drawClipCount;

						m_animationSampleJobsMapped[animJobBase + sampleJobsThisFrame] = animJob;

						m_skinCopyJobsMapped[animJobBase + skinJobCount] = AnimationContracts::SkinCopyJob{
						        .sampledPosesAddr = currSampledPosesAddr + static_cast<VkDeviceSize>(nodePoseCursor) * sizeof(AnimationContracts::SampledNodePose),
						        .dstPaletteOffset = skinPaletteOffset,
						        .jointCount = dc.skinJointCount,
						        .skinIndex = static_cast<std::uint32_t>(dc.skinIndex),
						        .nodeCount = drawNodeCount,
						        .nodePoseOffset = nodePoseCursor,
						};

						if (animSampleBatchCount > 0 && animSampleBatches[animSampleBatchCount - 1].db == drawAnimDb)
						{
							animSampleBatches[animSampleBatchCount - 1].count++;
						}
						else
						{
							AE_ASSERT_ALWAYS(animSampleBatchCount < 64, "RenderQueue: too many animation sample batches");
							animSampleBatches[animSampleBatchCount++] = {drawAnimDb, drawAnimDb ? drawAnimDb->GetGeneration() : 0, sampleJobsThisFrame, 1u};
						}

						if (skinPaletteBatchCount > 0 && skinPaletteBatches[skinPaletteBatchCount - 1].db == drawAnimDb)
						{
							skinPaletteBatches[skinPaletteBatchCount - 1].count++;
						}
						else
						{
							AE_ASSERT_ALWAYS(skinPaletteBatchCount < 64, "RenderQueue: too many skin palette batches");
							skinPaletteBatches[skinPaletteBatchCount++] = {drawAnimDb, drawAnimDb ? drawAnimDb->GetGeneration() : 0, skinJobCount, 1u};
						}

						++sampleJobsThisFrame;
						++skinJobCount;
						skinJointCursor += dc.skinJointCount;
						nodePoseCursor += drawNodeCount;
					}
				}

				m_instanceDataMapped[drawBase + globalDrawIdx] = DrawContracts::InstanceData{
				        .model = dc.modelMatrix,
				        .materialIndex = dc.materialIndex,
				        .skinPaletteOffset = skinPaletteOffset,
				        .skinJointCount = skinJointCount,
				        .worldBoundingSphere = dc.worldBoundingSphere,
				        .vertexBufferAddr = (dc.mesh != nullptr) ? dc.mesh->GetVertexDeviceAddress() : 0,
				};

				m_cullInputMapped[drawBase + globalDrawIdx] = CullContracts::DrawInput{
				        .indexCount = (dc.mesh != nullptr) ? dc.mesh->GetIndexCount() : 0u,
				        .instanceCount = 1u,
				        .firstIndex = 0u,
				        .vertexOffset = 0,
				        .firstInstance = globalDrawIdx, // frame-relative; BDA base (m_cachedInstanceDataAddr) accounts for slot
				        .batchIndex = batchIdx,
				};

				++globalDrawIdx;
			}

			m_batchDescMapped[batchBase + batchIdx] = CullContracts::Batch{batchOutputStart, batchDrawCount, batchOutputStart};

			m_batchRenderInfos.push_back(BatchRenderInfo{
			        .pipeline = batchPipeline,
			        .mesh = batchMesh,
			        .meshGeneration = batchMesh ? batchMesh->GetGeneration() : 0,
			        .outputStart = batchOutputStart,
			        .drawCount = batchDrawCount,
			});

			++batchIdx;
			i = batchEnd;
		}

		// Flush mapped writes before GPU reads.
		AE_EXPECT_OR_THROW_VOID(m_instanceDataBuffer.FlushMapped());
		AE_EXPECT_OR_THROW_VOID(m_cullInputBuffer.FlushMapped());
		AE_EXPECT_OR_THROW_VOID(m_batchDescBuffer.FlushMapped());
		if (m_skinCopyJobsMapped != nullptr)
		{
			AE_EXPECT_OR_THROW_VOID(m_skinCopyJobBuffer.FlushMapped());
		}
		if (sampleJobsThisFrame > 0)
		{
			AE_EXPECT_OR_THROW_VOID(m_animationSampleJobsBuffer.FlushMapped());
		}

		// Ensure host writes are visible to compute/graphics shader reads.
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

		if (sampleJobsThisFrame > 0 && !m_debugDisableAnimation)
		{
			AE_VERBOSE(LogCategory::Animation, "Animation dispatch enabled: {} sampleJobs, {} skinJobs", sampleJobsThisFrame, skinJobCount);
			// ── Pass 0: Parallel bind-pose initialization ──
			// Dispatched before animation sampling to write all node bind poses
			// in parallel (each thread handles one (job, node) pair).
			if ((m_debugAnimPassMask & 1u) && m_sharedPipelines != nullptr && m_sharedPipelines->poseInit != VK_NULL_HANDLE)
			{
				AE_PROFILE_ZONE_N("RenderQueue.Animation.PoseInit.Dispatch");
				AE_VERBOSE(LogCategory::Animation, "PoseInit: currSampledPosesAddr=0x{:x}, animJobsBDA=0x{:x}", currSampledPosesAddr, m_animationSampleJobsBuffer.GetDeviceAddress());

				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sharedPipelines->poseInit);
				CommandRecorder(cmd).BeginDebugLabel("Animation.PoseInit", 0.9f, 0.6f, 0.3f, 1.0f);

				const gpu::DeviceAddress animJobsBDAForInit = m_animationSampleJobsBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(animJobBase) * sizeof(AnimationContracts::AnimatorSampleJob);

				for (std::uint32_t bi = 0; bi < animSampleBatchCount; ++bi)
				{
					const auto& batch = animSampleBatches[bi];
					if (batch.db == nullptr || !batch.db->IsAlive() || batch.dbGeneration != batch.db->GetGeneration())
					{
						continue;
					}
					const std::uint32_t nodeCount = batch.db->GetNodeCount();
					if (nodeCount == 0)
					{
						continue;
					}
					AE_VERBOSE(LogCategory::Animation,
					        "  PoseInit Batch[{}]: nodeCount={} jobCount={} bindT=0x{:x} bindR=0x{:x} bindS=0x{:x}",
					        bi,
					        nodeCount,
					        batch.count,
					        batch.db->GetBindTranslationsAddr(),
					        batch.db->GetBindRotationsAddr(),
					        batch.db->GetBindScalesAddr());
					if (nodeCount == 0)
					{
						continue;
					}

					const AnimationContracts::PoseInitPush initPc{
					        .bindTranslationsAddr = batch.db->GetBindTranslationsAddr(),
					        .bindRotationsAddr = batch.db->GetBindRotationsAddr(),
					        .bindScalesAddr = batch.db->GetBindScalesAddr(),
					        .animatorJobsAddr = animJobsBDAForInit + static_cast<VkDeviceSize>(batch.startJob) * sizeof(AnimationContracts::AnimatorSampleJob),
					        .sampledPosesAddr = currSampledPosesAddr,
					        .jobCount = batch.count,
					        .nodeCountPerJob = nodeCount,
					};
					vkCmdPushConstants(cmd, m_sharedPipelines->poseInitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(initPc), &initPc);

					const std::uint32_t groupsX = (batch.count + 7u) / 8u;
					const std::uint32_t groupsY = (nodeCount + 7u) / 8u;
					vkCmdDispatch(cmd, groupsX, groupsY, 1);
				}

				const VkMemoryBarrier2 poseInitToAnim{
				        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				};
				const VkDependencyInfo poseInitToAnimDep{
				        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				        .memoryBarrierCount = 1,
				        .pMemoryBarriers = &poseInitToAnim,
				};
				vkCmdPipelineBarrier2(cmd, &poseInitToAnimDep);

				CommandRecorder(cmd).EndDebugLabel();
			}

			AE_PROFILE_ZONE_N("RenderQueue.Animation.SampleClips.Dispatch");

			if ((m_debugAnimPassMask & 2u) && m_sharedPipelines != nullptr && m_sharedPipelines->animSample != VK_NULL_HANDLE && sampleJobsThisFrame > 0)
			{
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sharedPipelines->animSample);
				CommandRecorder(cmd).BeginDebugLabel("Animation.SampleClips", 0.9f, 0.6f, 0.3f, 1.0f);

				const AnimationContracts::AnimationSamplePush animPc{
				        .animDbClipsAddr = 0,
				        .animDbChannelsAddr = 0,
				        .animDbTimesAddr = 0,
				        .animDbValuesAddr = 0,
				        .bindTranslationsAddr = 0,
				        .bindRotationsAddr = 0,
				        .bindScalesAddr = 0,
				        .animatorJobsAddr = m_animationSampleJobsBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(animJobBase) * sizeof(AnimationContracts::AnimatorSampleJob),
				        .sampledPosesAddr = currSampledPosesAddr,
				        .jobCount = sampleJobsThisFrame,
				        .clipCount = 0,
				};
				vkCmdPushConstants(cmd, m_sharedPipelines->animSampleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(animPc), &animPc);
				if (m_timestampPool)
				{
					m_tsSlots[frameSlot].animSampleStart = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}
				{
					AE_PROFILE_GPU_ZONE(m_tracyVkCtx, cmd, "Animation.SampleClips");
					const std::uint32_t groups = (sampleJobsThisFrame + 63u) / 64u;
					vkCmdDispatch(cmd, groups, 1, 1);
				}
				if (m_timestampPool)
				{
					m_tsSlots[frameSlot].animSampleEnd = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}

				CommandRecorder(cmd).EndDebugLabel();

				// Barrier: make GPU anim_sample writes visible to downstream
				// compute passes (node_flatten, build_skin_palette).
				const VkMemoryBarrier2 animToNodeFlatten{
				        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
				};
				const VkDependencyInfo animToNodeFlattenDep{
				        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				        .memoryBarrierCount = 1,
				        .pMemoryBarriers = &animToNodeFlatten,
				};
				vkCmdPipelineBarrier2(cmd, &animToNodeFlattenDep);
			}

			// ── Pass 1.3: Animation blend (cross-fade between two clips) ───────────
			if (m_animationBlendSystem != nullptr && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && (m_debugAnimPassMask & 2u))
			{
				const AnimationContracts::AnimationBlendPush& blendPc = m_animationBlendSystem->GetBlendPush();
				const std::uint32_t blendJobCount = m_animationBlendSystem->GetBlendJobCount();
				if (blendJobCount > 0 && blendPc.jobCount > 0)
				{
					AE_PROFILE_ZONE_N("RenderQueue.AnimationBlend.Dispatch");
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sharedPipelines->animBlend);
					CommandRecorder(cmd).BeginDebugLabel("Animation.AnimBlend", 0.6f, 0.4f, 0.8f, 1.0f);
					vkCmdPushConstants(cmd, m_sharedPipelines->animBlendLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blendPc), &blendPc);
					{
						AE_PROFILE_GPU_ZONE(m_tracyVkCtx, cmd, "Animation.AnimBlend");
						const std::uint32_t groups = (blendPc.jobCount + 63u) / 64u;
						vkCmdDispatch(cmd, groups, 1, 1);
					}
					CommandRecorder(cmd).EndDebugLabel();

					const VkMemoryBarrier2 blendToFlatten{
					        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
					        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
					};
					const VkDependencyInfo blendDep{
					        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					        .memoryBarrierCount = 1,
					        .pMemoryBarriers = &blendToFlatten,
					};
					vkCmdPipelineBarrier2(cmd, &blendDep);
				}
			}

			m_animationSampleJobCount = 0;
		}

		if (skinJobCount > 0 && m_debugLogSkinJobsFramesLeft > 0)
		{
			--m_debugLogSkinJobsFramesLeft;
			AE_VERBOSE(LogCategory::Animation, "RenderQueue SkinJob dump (frame {}, {} jobs, {} sampleJobs, {} anim batches, {} skin batches):", frameIndex, skinJobCount, sampleJobsThisFrame, animSampleBatchCount, skinPaletteBatchCount);
			AE_VERBOSE(LogCategory::Animation, "  dstPaletteAddr=0x{:x}", currSkinPaletteAddr);
			const AnimationDatabase* logDb = (skinPaletteBatchCount > 0) ? skinPaletteBatches[0].db : nullptr;
			AE_VERBOSE(LogCategory::Animation,
			        "  nodeParentsAddr=0x{:x}  skinMetasAddr=0x{:x}  skinJointsAddr=0x{:x}  skinInverseBindsAddr=0x{:x}",
			        logDb ? logDb->GetNodeParentsAddr() : 0,
			        logDb ? logDb->GetSkinMetasAddr() : 0,
			        logDb ? logDb->GetSkinJointsAddr() : 0,
			        logDb ? logDb->GetSkinInverseBindsAddr() : 0);
			const std::uint32_t logLimit = std::min(skinJobCount, 8u);
			for (std::uint32_t ji = 0; ji < logLimit; ++ji)
			{
				const AnimationContracts::SkinCopyJob& sj = m_skinCopyJobsMapped[animJobBase + ji];
				AE_VERBOSE(LogCategory::Animation, "  Job[{}]: dstOff={} joints={} skin={} nodes={} sampledAddr=0x{:x}", ji, sj.dstPaletteOffset, sj.jointCount, sj.skinIndex, sj.nodeCount, sj.sampledPosesAddr);
			}
			for (std::uint32_t ji = 0; ji < std::min(sampleJobsThisFrame, logLimit); ++ji)
			{
				const AnimationContracts::AnimatorSampleJob& aj = m_animationSampleJobsMapped[animJobBase + ji];
				AE_VERBOSE(LogCategory::Animation, "  SampleJob[{}]: clip={} time={:.3f} poseOff={} nodeCount={}", ji, aj.animClipIndex, aj.animTime, aj.nodePoseOffset, aj.nodeCount);
			}

			if (logDb && sampleJobsThisFrame > 0)
			{
				const AnimationContracts::AnimatorSampleJob& aj = m_animationSampleJobsMapped[animJobBase];
				const auto& bindT = logDb->GetBindTranslations();
				const auto& bindR = logDb->GetBindRotations();
				const auto& bindS = logDb->GetBindScales();
				const auto& parents = logDb->GetNodeParents();
				AE_VERBOSE(LogCategory::Animation, "  === BIND POSE DUMP (nodeCount={}) ===", aj.nodeCount);
				const std::uint32_t dumpLimit = std::min(aj.nodeCount, 10u);
				for (std::uint32_t n = 0; n < dumpLimit; ++n)
				{
					const auto& t = bindT[n];
					const auto& r = bindR[n];
					const auto& s = bindS[n];
					AE_VERBOSE(LogCategory::Animation, "    Node[{}]: T=[{:.2f},{:.2f},{:.2f}] R=[{:.3f},{:.3f},{:.3f},{:.3f}] S=[{:.2f},{:.2f},{:.2f}] parent={}", n, t.x, t.y, t.z, r.x, r.y, r.z, r.w, s.x, s.y, s.z, parents[n]);
				}
			}
		}

		// ── Pass 1.5: Flatten per-node global transforms (level-by-level depth dispatch) ──
		std::uint32_t firstBatchNodeCount = 0;
		if ((m_debugAnimPassMask & 4u) && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && m_sharedPipelines->nodeFlatten != VK_NULL_HANDLE)
		{
			AE_PROFILE_ZONE_N("RenderQueue.NodeFlatten.Dispatch");
			AE_VERBOSE(LogCategory::Animation, "NodeFlatten: {} batches, {} sampleJobs", animSampleBatchCount, sampleJobsThisFrame);

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sharedPipelines->nodeFlatten);
			CommandRecorder(cmd).BeginDebugLabel("Animation.NodeFlatten", 0.3f, 0.8f, 0.6f, 1.0f);

			const gpu::DeviceAddress animJobsBDA = m_animationSampleJobsBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(animJobBase) * sizeof(AnimationContracts::AnimatorSampleJob);

			for (std::uint32_t bi = 0; bi < animSampleBatchCount; ++bi)
			{
				const auto& batch = animSampleBatches[bi];
				if (batch.db == nullptr || !batch.db->IsAlive() || batch.dbGeneration != batch.db->GetGeneration())
				{
					continue;
				}
				const gpu::DeviceAddress depthAddr = batch.db->GetDepthRangesAddr();
				if (depthAddr == 0)
				{
					continue;
				}
				if (bi == 0 && m_animationIkSystem != nullptr)
				{
					firstBatchNodeCount = batch.db->GetNodeCount();
					m_animationIkSystem->SetDatabaseAddrs(batch.db->GetNodeParentsAddr(), batch.db->GetDepthSortedNodesAddr());
				}
				const std::uint32_t depthCount = batch.db->GetDepthCount();
				AE_VERBOSE(LogCategory::Animation, "  Batch[{}]: depthCount={} nodeParentsAddr=0x{:x} depthSortedNodesAddr=0x{:x}", bi, depthCount, batch.db->GetNodeParentsAddr(), batch.db->GetDepthSortedNodesAddr());
				if (depthCount == 0)
				{
					continue;
				}

				if (m_timestampPool && bi == 0 && skinJobCount > 0) // write start if we also have skin jobs
				{
					m_tsSlots[frameSlot].nodeFlattenStart = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}

				for (std::uint32_t di = 0; di < depthCount; ++di)
				{
					const AnimationDatabase::DepthRange& range = batch.db->GetDepthRange(di);
					if (range.count == 0)
					{
						continue;
					}

					const AnimationContracts::NodeFlattenPush flattenPc{
					        .sampledPosesAddr = currSampledPosesAddr,
					        .nodeParentsAddr = batch.db->GetNodeParentsAddr(),
					        .globalTransformsAddr = currNodeGlobalTransformsAddr,
					        .depthSortedNodesAddr = batch.db->GetDepthSortedNodesAddr(),
					        .animatorJobsAddr = animJobsBDA,
					        .depthOffset = range.startIndex,
					        .nodeCount = range.count,
					        .batchStartJob = batch.startJob,
					        .batchJobCount = batch.count,
					};
					vkCmdPushConstants(cmd, m_sharedPipelines->nodeFlattenLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(flattenPc), &flattenPc);

					const std::uint32_t totalWork = batch.count * range.count;
					const std::uint32_t groups = (totalWork + 63u) / 64u;
					vkCmdDispatch(cmd, groups, 1, 1);

					// Barrier between depth levels: parent writes from this
					// dispatch must be visible to the next level's reads.
					if (di + 1 < depthCount)
					{
						const VkMemoryBarrier2 depthBarrier{
						        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
						        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
						        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
						        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
						        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
						};
						const VkDependencyInfo depthBarrierDep{
						        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
						        .memoryBarrierCount = 1,
						        .pMemoryBarriers = &depthBarrier,
						};
						vkCmdPipelineBarrier2(cmd, &depthBarrierDep);
					}
				}

				if (m_timestampPool && bi == animSampleBatchCount - 1 && skinJobCount > 0)
				{
					m_tsSlots[frameSlot].nodeFlattenEnd = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}
			}
			CommandRecorder(cmd).EndDebugLabel();

			const VkMemoryBarrier2 flattenToSkin{
			        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			};
			const VkDependencyInfo flattenToSkinDep{
			        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			        .memoryBarrierCount = 1,
			        .pMemoryBarriers = &flattenToSkin,
			};
			vkCmdPipelineBarrier2(cmd, &flattenToSkinDep);
		}

		// ── Pass 1.8: IK solve (two-bone leg IK on GPU) ─────────────────────────
		if (m_animationIkSystem != nullptr && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && m_sharedPipelines->ikSolve != VK_NULL_HANDLE)
		{
			if (m_cachedNodeGlobalTransformsAddr != 0)
			{
				m_animationIkSystem->BuildIkSolvePush(m_cachedNodeGlobalTransformsAddr, 0, 0, firstBatchNodeCount);
			}
			const AnimationContracts::IkSolvePush& ikPc = m_animationIkSystem->GetIkSolvePush();
			const std::uint32_t ikJobCount = m_animationIkSystem->GetIkJobCount();
			if (ikJobCount > 0 && ikPc.jobCount > 0)
			{
				AE_PROFILE_ZONE_N("RenderQueue.IkSolve.Dispatch");
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sharedPipelines->ikSolve);
				CommandRecorder(cmd).BeginDebugLabel("Animation.IkSolve", 0.5f, 0.7f, 0.3f, 1.0f);
				vkCmdPushConstants(cmd, m_sharedPipelines->ikSolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ikPc), &ikPc);
				{
					AE_PROFILE_GPU_ZONE(m_tracyVkCtx, cmd, "Animation.IkSolve");
					const std::uint32_t groups = (ikPc.jobCount + 63u) / 64u;
					vkCmdDispatch(cmd, groups, 1, 1);
				}
				CommandRecorder(cmd).EndDebugLabel();

				const VkMemoryBarrier2 ikToSkin{
				        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
				};
				const VkDependencyInfo ikDep{
				        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				        .memoryBarrierCount = 1,
				        .pMemoryBarriers = &ikToSkin,
				};
				vkCmdPipelineBarrier2(cmd, &ikDep);
			}
		}

		if ((m_debugAnimPassMask & 8u) && skinJobCount > 0 && !m_debugDisableAnimation)
		{
			AE_PROFILE_ZONE_N("RenderQueue.Animation.BuildSkinPalette.Dispatch");

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sharedPipelines->skinCopy);
			CommandRecorder(cmd).BeginDebugLabel("Animation.BuildSkinPalette", 0.8f, 0.35f, 0.9f, 1.0f);

			for (std::uint32_t bi = 0; bi < skinPaletteBatchCount; ++bi)
			{
				const auto& batch = skinPaletteBatches[bi];
				if (batch.db == nullptr || !batch.db->IsAlive() || batch.dbGeneration != batch.db->GetGeneration())
				{
					continue;
				}
				const std::uint64_t skinMetasAddr = batch.db->GetSkinMetasAddr();
				if (skinMetasAddr == 0)
				{
					continue;
				}
				const AnimationContracts::SkinPalettePush skinPc{
				        .jobsAddr = m_skinCopyJobBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(animJobBase + batch.startJob) * sizeof(AnimationContracts::SkinCopyJob),
				        .dstPaletteAddr = currSkinPaletteAddr,
				        .globalTransformsAddr = currNodeGlobalTransformsAddr,
				        .skinMetasAddr = batch.db->GetSkinMetasAddr(),
				        .skinJointsAddr = batch.db->GetSkinJointsAddr(),
				        .skinInverseBindsAddr = batch.db->GetSkinInverseBindsAddr(),
				        .jobCount = batch.count,
				};
				vkCmdPushConstants(cmd, m_sharedPipelines->skinCopyLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(skinPc), &skinPc);
				if (m_timestampPool && bi == 0)
				{
					m_tsSlots[frameSlot].skinPaletteStart = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}
				{
					AE_PROFILE_GPU_ZONE(m_tracyVkCtx, cmd, "Animation.BuildSkinPalette");
					const std::uint32_t groups = (batch.count + 63u) / 64u;
					vkCmdDispatch(cmd, groups, 1, 1);
				}
				if (m_timestampPool && bi == skinPaletteBatchCount - 1)
				{
					m_tsSlots[frameSlot].skinPaletteEnd = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}
			}

			CommandRecorder(cmd).EndDebugLabel();

			const VkMemoryBarrier2 skinToShaders{
			        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
			        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			};
			const VkDependencyInfo skinToShadersDep{
			        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			        .memoryBarrierCount = 1,
			        .pMemoryBarriers = &skinToShaders,
			};
			vkCmdPipelineBarrier2(cmd, &skinToShadersDep);
		}
		else if (sampleJobsThisFrame > 0 && m_debugDisableAnimation)
		{
			AE_WARN(LogCategory::Animation, "Animation dispatch DISABLED by debug flag ({} sampleJobs, {} skinJobs skipped)", sampleJobsThisFrame, skinJobCount);
		}

		// ── Cull dispatch: single or multi-frustum ──
		const VkDeviceSize inputCmdOffset = static_cast<VkDeviceSize>(drawBase) * sizeof(CullContracts::DrawInput);
		const VkDeviceSize batchDescOffset = static_cast<VkDeviceSize>(batchBase) * sizeof(CullContracts::Batch);

		if (m_outputDrawCapacity > m_maxDraws)
		{
			// Multi-frustum mode (shadow cascades): test each draw against 3 VP matrices,
			// write 3 independent output regions.  The 3 frame constant BDAs must have
			// been set via SetMultiCullFrameAddrs() before this call.
			const VkDeviceSize outputBase = static_cast<VkDeviceSize>(frameSlot) * m_outputDrawCapacity;
			const VkDeviceSize outputCmdOffset = outputBase * sizeof(VkDrawIndexedIndirectCommand);
			const VkDeviceSize cascadeStride = static_cast<VkDeviceSize>(m_maxDraws) * sizeof(VkDrawIndexedIndirectCommand);

			const CullContracts::MultiPushConstants multiPc{
			        .frameAddrs = {m_multiFrameAddrs[0], m_multiFrameAddrs[1], m_multiFrameAddrs[2]},
			        .instanceDataAddr = m_cachedInstanceDataAddr,
			        .inputCmdAddr = m_cullInputBuffer.GetDeviceAddress() + inputCmdOffset,
			        .outputCmdAddr = m_outputIndirectBuffer.GetDeviceAddress() + outputCmdOffset,
			        .batchDescAddr = m_batchDescBuffer.GetDeviceAddress() + batchDescOffset,
			        .totalDrawCount = totalDraws,
			        .outputCascadeStride = static_cast<std::uint32_t>(cascadeStride),
			        .debugFlags = m_debugForceVisible ? CullContracts::kDebugForceVisibleBit : 0u,
			};

			{
				AE_PROFILE_ZONE_N("RenderQueue.Cull.DispatchMulti");
				CommandRecorder(cmd).BeginDebugLabel("CullPass.cullDrawsMulti", 0.4f, 0.8f, 0.4f, 1.0f);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
				vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(multiPc), &multiPc);
				const std::uint32_t groups = (totalDraws + 63u) / 64u;
				vkCmdDispatch(cmd, groups, 1, 1);
				CommandRecorder(cmd).EndDebugLabel();
			}
		}
		else
		{
			// Single-frustum mode (main camera, local shadows).
			const VkDeviceSize outputCmdOffset = static_cast<VkDeviceSize>(drawBase) * sizeof(VkDrawIndexedIndirectCommand);
			const CullContracts::PushConstants pc{
			        .frameAddr = frameAddr,
			        .instanceDataAddr = m_cachedInstanceDataAddr,
			        .inputCmdAddr = m_cullInputBuffer.GetDeviceAddress() + inputCmdOffset,
			        .outputCmdAddr = m_outputIndirectBuffer.GetDeviceAddress() + outputCmdOffset,
			        .batchDescAddr = m_batchDescBuffer.GetDeviceAddress() + batchDescOffset,
			        .batchCountAddr = 0,
			        .totalDrawCount = totalDraws,
			        .debugFlags = m_debugForceVisible ? CullContracts::kDebugForceVisibleBit : 0u,
			};

			{
				AE_PROFILE_ZONE_N("RenderQueue.Cull.Dispatch");
				CommandRecorder(cmd).BeginDebugLabel("CullPass.cullDraws", 0.4f, 0.8f, 0.4f, 1.0f);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
				vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
				if (m_timestampPool)
				{
					m_tsSlots[frameSlot].cullStart = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}
				{
					AE_PROFILE_GPU_ZONE(m_tracyVkCtx, cmd, "CullPass.cullDraws");
					const std::uint32_t groups = (totalDraws + 63u) / 64u;
					vkCmdDispatch(cmd, groups, 1, 1);
				}
				if (m_timestampPool)
				{
					m_tsSlots[frameSlot].cullEnd = m_timestampPool->Write(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				}
				CommandRecorder(cmd).EndDebugLabel();
			}
		}

		// Ensure indirect args are visible before draw-indirect.
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

#ifdef TRACY_ENABLE
		{
			TracyPlot("Animation/SampleJobs", static_cast<int64_t>(sampleJobsThisFrame));
			TracyPlot("Animation/SkinCopyJobs", static_cast<int64_t>(skinJobCount));
			TracyPlot("RenderQueue/TotalDraws", static_cast<int64_t>(totalDraws));
		}
		AE_PROFILE_GPU_COLLECT(m_tracyVkCtx, cmd);
#endif
	}

	void RenderQueue::FlushDraw(gpu::CommandList& cmd, gpu::DescriptorSet bindlessSet, gpu::DescriptorSet lightingSet, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset)
	{
		FlushDrawImpl(cmd, bindlessSet, lightingSet, m_cachedFrameAddr, overridePipeline, cascadeOffset, "RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f);
	}

	void RenderQueue::FlushDrawPush(gpu::CommandList& cmd, gpu::DescriptorSet bindlessSet, std::function<void(gpu::CommandList&, gpu::PipelineLayout)> pushLightingFn, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset)
	{
		FlushDrawImpl(cmd, bindlessSet, nullptr, m_cachedFrameAddr, overridePipeline, cascadeOffset, "RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f, pushLightingFn);
	}

	void RenderQueue::FlushDrawWithFrameAddr(gpu::CommandList& cmd, gpu::DescriptorSet bindlessSet, gpu::DescriptorSet lightingSet, const gpu::DeviceAddress overrideFrameAddr, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset)
	{
		FlushDrawImpl(cmd, bindlessSet, lightingSet, overrideFrameAddr, overridePipeline, cascadeOffset, "RenderQueue.FlushDrawWithAddr", 0.85f, 0.40f, 0.60f);
	}

	void RenderQueue::FlushDrawImpl(gpu::CommandList& cmd,
	        gpu::DescriptorSet bindlessSet,
	        gpu::DescriptorSet lightingSet,
	        gpu::DeviceAddress frameAddr,
	        const GraphicsPipeline* overridePipeline,
	        std::uint32_t cascadeOffset,
	        const char* debugLabel,
	        float r,
	        float g,
	        float b,
	        const LightingPushFn& pushLightingFn)
	{
		AE_PROFILE_ZONE();
		if (!cmd.IsValid())
		{
			return;
		}
		if (m_batchRenderInfos.empty())
		{
			return;
		}

		cmd.BeginDebugLabel(debugLabel, r, g, b, 1.0f);

		const DrawContracts::PushConstants sharedPc{
		        .frameAddr = frameAddr,
		        .instanceDataAddr = m_cachedInstanceDataAddr,
		        .skinPaletteAddr = m_cachedSkinPaletteAddr,
		};
		AE_VERBOSE(LogCategory::Render, "FlushDraw: frameAddr=0x{:x}, instanceDataAddr=0x{:x}, skinPaletteAddr=0x{:x}, batches={}", frameAddr, m_cachedInstanceDataAddr, m_cachedSkinPaletteAddr, m_batchRenderInfos.size());

		const GraphicsPipeline* lastPipeline = nullptr;
		const Mesh* lastMesh = nullptr;
		VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
		VkDeviceSize lastIndexOffset = ~0ull;
		const GraphicsPipeline* lastSetPipeline = nullptr;
		const GraphicsPipeline* lastLightingSetPipeline = nullptr;

		for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(m_batchRenderInfos.size()); ++bi)
		{
			const BatchRenderInfo& batch = m_batchRenderInfos[bi];
			const GraphicsPipeline* activePipeline = overridePipeline != nullptr ? overridePipeline : batch.pipeline;

			if (activePipeline != nullptr && activePipeline != lastPipeline)
			{
				cmd.BindPipeline(activePipeline->GetPipeline(), activePipeline->GetLayout());
				lastPipeline = activePipeline;
				lastSetPipeline = nullptr;
				lastLightingSetPipeline = nullptr;
			}

			if (bindlessSet != nullptr && activePipeline != nullptr && activePipeline != lastSetPipeline)
			{
				cmd.BindDescriptorSet(static_cast<gpu::PipelineLayout>(activePipeline->GetLayout()), 0, bindlessSet);
				lastSetPipeline = activePipeline;
			}

			if (pushLightingFn && activePipeline != nullptr && activePipeline->GetSetLayoutCount() > 1 && activePipeline != lastLightingSetPipeline)
			{
				pushLightingFn(cmd, static_cast<gpu::PipelineLayout>(activePipeline->GetLayout()));
				lastLightingSetPipeline = activePipeline;
			}
			else if (lightingSet != nullptr && activePipeline != nullptr && activePipeline->GetSetLayoutCount() > 1 && activePipeline != lastLightingSetPipeline)
			{
				cmd.BindDescriptorSet(static_cast<gpu::PipelineLayout>(activePipeline->GetLayout()), 1, lightingSet);
				lastLightingSetPipeline = activePipeline;
			}

			if (activePipeline != nullptr)
			{
				cmd.PushConstantsRaw(activePipeline->GetLayout(), gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, gpu::AsPushConstantBytes(sharedPc));
			}

			if (batch.mesh != nullptr && batch.mesh->IsAlive() && batch.mesh->IsValid() && batch.mesh->GetGeneration() == batch.meshGeneration && batch.mesh->GetIndexBuffer() != VK_NULL_HANDLE)
			{
				const VkBuffer indexBuffer = batch.mesh->GetIndexBuffer();
				const VkDeviceSize indexOffset = batch.mesh->GetIndexByteOffset();
				if (indexBuffer != lastIndexBuffer || indexOffset != lastIndexOffset)
				{
					cmd.BindIndexBuffer(indexBuffer, indexOffset);
					lastIndexBuffer = indexBuffer;
					lastIndexOffset = indexOffset;
				}
				lastMesh = batch.mesh;
			}

			if (m_debugBypassIndirect)
			{
				const std::uint32_t indexCount = (batch.mesh != nullptr && batch.mesh->IsAlive() && batch.mesh->IsValid() && batch.mesh->GetGeneration() == batch.meshGeneration) ? batch.mesh->GetIndexCount() : 0u;
				for (std::uint32_t local = 0; local < batch.drawCount; ++local)
				{
					cmd.DrawIndexed(indexCount, 1u, 0u, 0, batch.outputStart + local);
				}
			}
			else
			{
				cmd.DrawIndexedIndirect(m_outputIndirectBuffer.Get(), static_cast<VkDeviceSize>(m_cachedDrawBase + cascadeOffset + batch.outputStart) * sizeof(VkDrawIndexedIndirectCommand), batch.drawCount, sizeof(VkDrawIndexedIndirectCommand));
			}
		}

		cmd.EndDebugLabel();
	}

	void RenderQueue::Clear(std::uint32_t slot)
	{
		m_commandSlots[slot % kFramesInFlight].clear();
		m_batchRenderInfos.clear();
	}

	bool RenderQueue::IsEmpty(std::uint32_t slot) const
	{
		return m_commandSlots[slot % kFramesInFlight].empty();
	}

	void RenderQueueSharedPipelines::Initialize(VkDevice device, VkPipelineCache pipelineCache)
	{
		{
			AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://skin_palette_build.spv"));
			AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "RenderQueueShared"));

			const VkPushConstantRange pushRange{
			        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			        .offset = 0,
			        .size = sizeof(AnimationContracts::SkinPalettePush), // 72 bytes
			};
			const VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			        .pushConstantRangeCount = 1,
			        .pPushConstantRanges = &pushRange,
			};
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &skinCopyLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create skin copy pipeline layout."));
			}

			const VkPipelineShaderStageCreateInfo stage{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module = shaderModule,
			        .pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo{
			        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			        .stage = stage,
			        .layout = skinCopyLayout,
			};
			if (vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &skinCopy) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create skin copy compute pipeline."));
			}

			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(skinCopy), VK_OBJECT_TYPE_PIPELINE, "Animation.BuildSkinPalette");
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(skinCopyLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Animation.BuildSkinPalette.Layout");

			vkDestroyShaderModule(device, shaderModule, nullptr);
		}

		{
			AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://animation_sample.spv"));
			AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "RenderQueueShared"));

			const VkPushConstantRange pushRange{
			        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			        .offset = 0,
			        .size = sizeof(AnimationContracts::AnimationSamplePush),
			};
			const VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			        .pushConstantRangeCount = 1,
			        .pPushConstantRanges = &pushRange,
			};
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &animSampleLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create animation sample pipeline layout."));
			}

			const VkPipelineShaderStageCreateInfo stage{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module = shaderModule,
			        .pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo{
			        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			        .stage = stage,
			        .layout = animSampleLayout,
			};
			if (vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &animSample) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create animation sample compute pipeline."));
			}

			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(animSample), VK_OBJECT_TYPE_PIPELINE, "Animation.SampleClips");
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(animSampleLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Animation.SampleClips.Layout");

			vkDestroyShaderModule(device, shaderModule, nullptr);
		}

		{
			AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://pose_init.spv"));
			AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "RenderQueueShared"));

			const VkPushConstantRange pushRange{
			        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			        .offset = 0,
			        .size = sizeof(AnimationContracts::PoseInitPush),
			};
			const VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			        .pushConstantRangeCount = 1,
			        .pPushConstantRanges = &pushRange,
			};
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &poseInitLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create pose init pipeline layout."));
			}

			const VkPipelineShaderStageCreateInfo stage{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module = shaderModule,
			        .pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo{
			        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			        .stage = stage,
			        .layout = poseInitLayout,
			};
			if (vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &poseInit) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create pose init compute pipeline."));
			}

			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(poseInit), VK_OBJECT_TYPE_PIPELINE, "Animation.PoseInit");
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(poseInitLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Animation.PoseInit.Layout");

			vkDestroyShaderModule(device, shaderModule, nullptr);
		}

		{
			AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://node_flatten.spv"));
			AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "RenderQueueShared"));

			const VkPushConstantRange pushRange{
			        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			        .offset = 0,
			        .size = sizeof(AnimationContracts::NodeFlattenPush),
			};
			const VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			        .pushConstantRangeCount = 1,
			        .pPushConstantRanges = &pushRange,
			};
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &nodeFlattenLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create nodeFlatten pipeline layout."));
			}

			const VkPipelineShaderStageCreateInfo stage{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module = shaderModule,
			        .pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo{
			        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			        .stage = stage,
			        .layout = nodeFlattenLayout,
			};
			if (vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &nodeFlatten) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create nodeFlatten compute pipeline."));
			}

			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(nodeFlatten), VK_OBJECT_TYPE_PIPELINE, "Animation.NodeFlatten");
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(nodeFlattenLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Animation.NodeFlatten.Layout");

			vkDestroyShaderModule(device, shaderModule, nullptr);
		}

		{
			AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://anim_blend.spv"));
			AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "RenderQueueShared"));

			const VkPushConstantRange pushRange{
			        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			        .offset = 0,
			        .size = sizeof(AnimationContracts::AnimationBlendPush),
			};
			const VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			        .pushConstantRangeCount = 1,
			        .pPushConstantRanges = &pushRange,
			};
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &animBlendLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create animBlend pipeline layout."));
			}

			const VkPipelineShaderStageCreateInfo stage{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module = shaderModule,
			        .pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo{
			        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			        .stage = stage,
			        .layout = animBlendLayout,
			};
			if (vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &animBlend) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create animBlend compute pipeline."));
			}

			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(animBlend), VK_OBJECT_TYPE_PIPELINE, "Animation.AnimBlend");
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(animBlendLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Animation.AnimBlend.Layout");

			vkDestroyShaderModule(device, shaderModule, nullptr);
		}

		{
			AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://ik_solve.spv"));
			AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "RenderQueueShared"));

			const VkPushConstantRange pushRange{
			        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			        .offset = 0,
			        .size = sizeof(AnimationContracts::IkSolvePush),
			};
			const VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			        .pushConstantRangeCount = 1,
			        .pPushConstantRanges = &pushRange,
			};
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &ikSolveLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create ikSolve pipeline layout."));
			}

			const VkPipelineShaderStageCreateInfo stage{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			        .module = shaderModule,
			        .pName = "main",
			};
			const VkComputePipelineCreateInfo pipelineInfo{
			        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			        .stage = stage,
			        .layout = ikSolveLayout,
			};
			if (vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &ikSolve) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create ikSolve compute pipeline."));
			}

			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(ikSolve), VK_OBJECT_TYPE_PIPELINE, "Animation.IkSolve");
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(ikSolveLayout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Animation.IkSolve.Layout");

			vkDestroyShaderModule(device, shaderModule, nullptr);
		}
	}

	void RenderQueueSharedPipelines::Shutdown(VkDevice device)
	{
		if (skinCopy != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, skinCopy, nullptr);
			skinCopy = VK_NULL_HANDLE;
		}
		if (skinCopyLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, skinCopyLayout, nullptr);
			skinCopyLayout = VK_NULL_HANDLE;
		}
		if (animSample != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, animSample, nullptr);
			animSample = VK_NULL_HANDLE;
		}
		if (animSampleLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, animSampleLayout, nullptr);
			animSampleLayout = VK_NULL_HANDLE;
		}
		if (nodeFlatten != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, nodeFlatten, nullptr);
			nodeFlatten = VK_NULL_HANDLE;
		}
		if (nodeFlattenLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, nodeFlattenLayout, nullptr);
			nodeFlattenLayout = VK_NULL_HANDLE;
		}
		if (poseInit != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, poseInit, nullptr);
			poseInit = VK_NULL_HANDLE;
		}
		if (poseInitLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, poseInitLayout, nullptr);
			poseInitLayout = VK_NULL_HANDLE;
		}
		if (animBlend != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, animBlend, nullptr);
			animBlend = VK_NULL_HANDLE;
		}
		if (animBlendLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, animBlendLayout, nullptr);
			animBlendLayout = VK_NULL_HANDLE;
		}
		if (ikSolve != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, ikSolve, nullptr);
			ikSolve = VK_NULL_HANDLE;
		}
		if (ikSolveLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, ikSolveLayout, nullptr);
			ikSolveLayout = VK_NULL_HANDLE;
		}
	}
} // namespace aether
