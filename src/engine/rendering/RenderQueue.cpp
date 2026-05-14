#include "rendering/RenderQueue.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "rendering/CommandRecorder.hpp"
#include "FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "mesh/Mesh.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	void RenderQueue::Initialize(VkDevice device, VmaAllocator allocator, std::uint32_t maxDraws, std::uint32_t maxBatches, std::uint32_t maxAnimationDraws)
	{
		m_device = device;
		m_allocator = allocator;
		m_maxDraws = maxDraws;
		m_maxBatches = maxBatches;
		m_maxAnimationDraws = (maxAnimationDraws == UINT32_MAX) ? std::min(maxDraws, kDefaultMaxAnimationDraws) : maxAnimationDraws;
		m_maxSkinJoints = m_maxAnimationDraws * 128u;
		m_maxSampledPoses = m_maxSkinJoints * 2u;

		constexpr VkBufferUsageFlags kSsboFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		AE_EXPECT_OR_THROW(b0, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxDraws) * sizeof(DrawInstanceData), kSsboFlags));
		m_instanceDataBuffer = std::move(*b0);
		m_instanceDataMapped = static_cast<DrawInstanceData*>(m_instanceDataBuffer.GetAllocationInfo().pMappedData);

		AE_EXPECT_OR_THROW(b1, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxDraws) * sizeof(CullDrawInput), kSsboFlags));
		m_cullInputBuffer = std::move(*b1);
		m_cullInputMapped = static_cast<CullDrawInput*>(m_cullInputBuffer.GetAllocationInfo().pMappedData);

		AE_EXPECT_OR_THROW(b2, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxBatches) * sizeof(CullBatch), kSsboFlags));
		m_batchDescBuffer = std::move(*b2);
		m_batchDescMapped = static_cast<CullBatch*>(m_batchDescBuffer.GetAllocationInfo().pMappedData);

		if (m_maxAnimationDraws > 0u)
		{
			AE_EXPECT_OR_THROW(b3, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxAnimationDraws) * sizeof(SkinCopyJob), kSsboFlags));
			m_skinCopyJobBuffer = std::move(*b3);
			m_skinCopyJobsMapped = static_cast<SkinCopyJob*>(m_skinCopyJobBuffer.GetAllocationInfo().pMappedData);

			AE_EXPECT_OR_THROW(b4, UniqueBuffer::CreateMapped(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxAnimationDraws) * sizeof(AnimatorSampleJob), kSsboFlags));
			m_animationSampleJobsBuffer = std::move(*b4);
			m_animationSampleJobsMapped = static_cast<AnimatorSampleJob*>(m_animationSampleJobsBuffer.GetAllocationInfo().pMappedData);

			AE_EXPECT_OR_THROW(b5, UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));
			m_skinPaletteBuffer = std::move(*b5);

			AE_EXPECT_OR_THROW(b6, UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(SampledNodePose), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));
			m_sampledPosesBuffer = std::move(*b6);
		}

		AE_EXPECT_OR_THROW(b7, UniqueBuffer::CreateDeviceLocal(allocator, device, kFramesInFlight * static_cast<VkDeviceSize>(maxDraws) * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));
		m_outputIndirectBuffer = std::move(*b7);
		EnsureSkinCopyPipeline();
	}

	void RenderQueue::Shutdown()
	{
		if (m_skinCopyPipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, m_skinCopyPipeline, nullptr);
			m_skinCopyPipeline = VK_NULL_HANDLE;
		}
		if (m_skinCopyPipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(m_device, m_skinCopyPipelineLayout, nullptr);
			m_skinCopyPipelineLayout = VK_NULL_HANDLE;
		}
		if (m_animationSamplePipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, m_animationSamplePipeline, nullptr);
			m_animationSamplePipeline = VK_NULL_HANDLE;
		}
		if (m_animationSamplePipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(m_device, m_animationSamplePipelineLayout, nullptr);
			m_animationSamplePipelineLayout = VK_NULL_HANDLE;
		}

		m_sampledPosesBuffer.Reset();
		m_animationSampleJobsBuffer.Reset();
		m_outputIndirectBuffer.Reset();
		m_skinPaletteBuffer.Reset();
		m_skinCopyJobBuffer.Reset();
		m_batchDescBuffer.Reset();
		m_cullInputBuffer.Reset();
		m_instanceDataBuffer.Reset();
		m_animationSampleJobsMapped = nullptr;
		m_skinCopyJobsMapped = nullptr;
		m_instanceDataMapped = nullptr;
		m_cullInputMapped = nullptr;
		m_batchDescMapped = nullptr;
		m_maxDraws = 0;
		m_maxBatches = 0;
		m_maxAnimationDraws = 0;
		m_maxSkinJoints = 0;
		m_maxSampledPoses = 0;
		m_allocator = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
	}

	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commandSlots[m_writeSlot].push_back(cmd);
	}

	void RenderQueue::PrepareAndDispatch(VkCommandBuffer cmd, VkDeviceAddress frameAddr, VkPipeline computePipeline, VkPipelineLayout computeLayout, std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
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
		m_cachedDrawBase = drawBase;
		m_cachedBatchBase = batchBase;
		m_cachedInstanceDataAddr = m_instanceDataBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(drawBase) * sizeof(DrawInstanceData);
		const std::uint32_t prevFrameSlot = (frameSlot + kFramesInFlight - 1u) % kFramesInFlight;
		const VkDeviceAddress currSkinPaletteAddr = (m_maxSkinJoints > 0u) ? m_skinPaletteBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(frameSlot) * static_cast<VkDeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4) : 0;
		const VkDeviceAddress prevSkinPaletteAddr = (m_maxSkinJoints > 0u) ? m_skinPaletteBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(prevFrameSlot) * static_cast<VkDeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4) : 0;
		const VkDeviceAddress currSampledPosesAddr = (m_maxSampledPoses > 0u) ? m_sampledPosesBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(frameSlot) * static_cast<VkDeviceSize>(m_maxSampledPoses) * sizeof(SampledNodePose) : 0;
		m_cachedSkinPaletteAddr = currSkinPaletteAddr;
		const bool gpuSamplingEnabled = !m_debugForceCpuSkinFallback && m_animationDb != nullptr && m_animationDb->IsValid() && m_animationSampleJobsMapped != nullptr && m_skinCopyJobsMapped != nullptr && m_maxAnimationDraws > 0u;
		const std::uint32_t animClipCount = (m_animationDb != nullptr) ? m_animationDb->GetClipCount() : 0u;
		const std::uint32_t animNodeCount = (m_animationDb != nullptr) ? m_animationDb->GetNodeCount() : 0u;
		const std::uint32_t animSkinCount = (m_animationDb != nullptr) ? m_animationDb->GetSkinCount() : 0u;

		std::unordered_map<VkDeviceAddress, std::uint32_t> paletteOffsets;
		std::uint32_t skinJointCursor = 0;
		std::uint32_t nodePoseCursor = 0;
		std::uint32_t skinJobCount = 0;
		std::uint32_t sampleJobsThisFrame = 0;

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
		assert(totalDraws <= m_maxDraws && "RenderQueue: exceeded maxDraws - increase Initialize capacity.");

		std::uint32_t globalDrawIdx = 0; // monotonically increasing index within this frame slot
		std::uint32_t batchIdx = 0;

		for (std::size_t i = 0; i < m_commands.size();)
		{
			const GraphicsPipeline* batchPipeline = m_commands[i].pipeline;
			const Mesh* batchMesh = m_commands[i].mesh;

			const std::uint32_t batchOutputStart = globalDrawIdx;

			std::size_t batchEnd = i;
			while (batchEnd < m_commands.size() && m_commands[batchEnd].pipeline == batchPipeline && m_commands[batchEnd].mesh == batchMesh)
			{
				++batchEnd;
			}
			const std::uint32_t batchDrawCount = static_cast<std::uint32_t>(batchEnd - i);

			assert(batchIdx < m_maxBatches && "RenderQueue: exceeded maxBatches - increase Initialize capacity.");

			for (std::size_t j = i; j < batchEnd; ++j)
			{
				const DrawCommand& dc = m_commands[j];

				std::uint32_t skinPaletteOffset = 0;
				std::uint32_t skinJointCount = 0;
				const bool wantsGpuSampling = gpuSamplingEnabled && dc.gpuSampleEligible && dc.skinJointCount > 0 && dc.animClipIndex < animClipCount && dc.skinIndex >= 0 && static_cast<std::uint32_t>(dc.skinIndex) < animSkinCount;
				if (dc.skinJointCount > 0)
				{
					bool wroteSkinJob = false;

					if (wantsGpuSampling)
					{
						if (skinJointCursor + dc.skinJointCount > m_maxSkinJoints)
						{
							WARN(LogCategory::Engine, "RenderQueue: sampled skin palette pool overflow (needed {}, cap {}) - dropping GPU skinning for this draw.", skinJointCursor + dc.skinJointCount, m_maxSkinJoints);
						}
						else if (animNodeCount == 0u || nodePoseCursor + animNodeCount > m_maxSampledPoses)
						{
							WARN(LogCategory::Engine, "RenderQueue: sampled node-pose pool overflow (needed {}, cap {}) - dropping GPU skinning for this draw.", nodePoseCursor + animNodeCount, m_maxSampledPoses);
						}
						else if (skinJobCount >= m_maxAnimationDraws || m_animationSampleJobCount >= m_maxAnimationDraws)
						{
							WARN(LogCategory::Engine, "RenderQueue: animation job overflow (jobs {}, cap {}) - dropping GPU skinning for this draw.", std::max(skinJobCount, m_animationSampleJobCount), m_maxAnimationDraws);
						}
						else
						{
							skinPaletteOffset = skinJointCursor;
							skinJointCount = dc.skinJointCount;
							m_animationSampleJobsMapped[animJobBase + m_animationSampleJobCount] = AnimatorSampleJob{
								.animClipIndex = dc.animClipIndex,
								.animTime = dc.animTime,
								.nodePoseOffset = nodePoseCursor,
								.nodeCount = animNodeCount,
							};
							m_skinCopyJobsMapped[animJobBase + skinJobCount] = SkinCopyJob{
								.srcPaletteAddr = dc.sourceSkinBufferAddr,
								.sampledPosesAddr = currSampledPosesAddr + static_cast<VkDeviceSize>(nodePoseCursor) * sizeof(SampledNodePose),
								.dstPaletteOffset = skinPaletteOffset,
								.jointCount = dc.skinJointCount,
								.skinIndex = static_cast<std::uint32_t>(dc.skinIndex),
								.nodeCount = animNodeCount,
								.blendAlpha = dc.nonHeroGpuBlend ? 0.45f : 1.0f,
								.useSampledPoses = 1u,
							};
							++m_animationSampleJobCount;
							++sampleJobsThisFrame;
							++skinJobCount;
							skinJointCursor += dc.skinJointCount;
							nodePoseCursor += animNodeCount;
							wroteSkinJob = true;
						}
					}

					if (!wroteSkinJob && dc.sourceSkinBufferAddr != 0)
					{
						auto it = paletteOffsets.find(dc.sourceSkinBufferAddr);
						if (it == paletteOffsets.end())
						{
							if (skinJointCursor + dc.skinJointCount > m_maxSkinJoints)
							{
								WARN(LogCategory::Engine, "RenderQueue: skin palette pool overflow (needed {}, cap {}) - dropping skinning for this draw.", skinJointCursor + dc.skinJointCount, m_maxSkinJoints);
							}
							else
							{
								skinPaletteOffset = skinJointCursor;
								skinJointCount = dc.skinJointCount;
								paletteOffsets.emplace(dc.sourceSkinBufferAddr, skinPaletteOffset);
								if (skinJobCount < m_maxAnimationDraws)
								{
									m_skinCopyJobsMapped[animJobBase + skinJobCount] = SkinCopyJob{
										.srcPaletteAddr = dc.sourceSkinBufferAddr,
										.sampledPosesAddr = 0,
										.dstPaletteOffset = skinPaletteOffset,
										.jointCount = dc.skinJointCount,
										.skinIndex = 0,
										.nodeCount = 0,
										.blendAlpha = dc.nonHeroGpuBlend ? 0.45f : 1.0f,
										.useSampledPoses = 0u,
									};
									++skinJobCount;
								}
								skinJointCursor += dc.skinJointCount;
							}
						}
						else
						{
							skinPaletteOffset = it->second;
							skinJointCount = dc.skinJointCount;
						}
					}
				}

				m_instanceDataMapped[drawBase + globalDrawIdx] = DrawInstanceData{
					.model = dc.modelMatrix,
					.materialIndex = dc.materialIndex,
					.skinPaletteOffset = skinPaletteOffset,
					.skinJointCount = skinJointCount,
					.worldBoundingSphere = dc.worldBoundingSphere,
					.vertexBufferAddr = (dc.mesh != nullptr) ? dc.mesh->GetVertexDeviceAddress() : 0,
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

		// Flush mapped writes before GPU reads.
		vmaFlushAllocation(m_allocator, m_instanceDataBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
		vmaFlushAllocation(m_allocator, m_cullInputBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
		vmaFlushAllocation(m_allocator, m_batchDescBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
		if (m_skinCopyJobsMapped != nullptr)
		{
			vmaFlushAllocation(m_allocator, m_skinCopyJobBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
		}

		if (sampleJobsThisFrame > 0 && m_animationDb != nullptr && m_animationDb->IsValid())
		{
			vmaFlushAllocation(m_allocator, m_animationSampleJobsBuffer.GetAllocation(), 0, VK_WHOLE_SIZE);
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

		if (sampleJobsThisFrame > 0 && m_animationDb != nullptr && m_animationDb->IsValid())
		{
			AE_PROFILE_ZONE_N("RenderQueue.Animation.SampleClips.Dispatch");

			EnsureAnimationSamplePipeline();

			if (m_animationSamplePipeline != VK_NULL_HANDLE)
			{
				const AnimationSamplePush animPc{
					.animDbClipsAddr = m_animationDb->GetClipsAddr(),
					.animDbChannelsAddr = m_animationDb->GetChannelsAddr(),
					.animDbTimesAddr = m_animationDb->GetTimesAddr(),
					.animDbValuesAddr = m_animationDb->GetValuesAddr(),
					.bindTranslationsAddr = m_animationDb->GetBindTranslationsAddr(),
					.bindRotationsAddr = m_animationDb->GetBindRotationsAddr(),
					.bindScalesAddr = m_animationDb->GetBindScalesAddr(),
					.animatorJobsAddr = m_animationSampleJobsBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(animJobBase) * sizeof(AnimatorSampleJob),
					.sampledPosesAddr = currSampledPosesAddr,
					.jobCount = sampleJobsThisFrame,
					.clipCount = m_animationDb->GetClipCount(),
				};

				CommandRecorder(cmd).BeginDebugLabel("Animation.SampleClips", 0.9f, 0.6f, 0.3f, 1.0f);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_animationSamplePipeline);
				vkCmdPushConstants(cmd, m_animationSamplePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(animPc), &animPc);
				const std::uint32_t groups = (sampleJobsThisFrame + 63u) / 64u;
				vkCmdDispatch(cmd, groups, 1, 1);
				CommandRecorder(cmd).EndDebugLabel();

				const VkMemoryBarrier2 animToSkin{
					.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
					.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
				};
				const VkDependencyInfo animToSkinDep{
					.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.memoryBarrierCount = 1,
					.pMemoryBarriers = &animToSkin,
				};
				vkCmdPipelineBarrier2(cmd, &animToSkinDep);
			}

			m_animationSampleJobCount = 0;
		}

		if (skinJobCount > 0)
		{
			AE_PROFILE_ZONE_N("RenderQueue.Animation.BuildSkinPalette.Dispatch");
			const SkinPalettePush skinPc{
				.jobsAddr = m_skinCopyJobBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(animJobBase) * sizeof(SkinCopyJob),
				.dstPaletteAddr = currSkinPaletteAddr,
				.prevPaletteAddr = prevSkinPaletteAddr,
				.nodeParentsAddr = m_animationDb != nullptr ? m_animationDb->GetNodeParentsAddr() : 0,
				.skinMetasAddr = m_animationDb != nullptr ? m_animationDb->GetSkinMetasAddr() : 0,
				.skinJointsAddr = m_animationDb != nullptr ? m_animationDb->GetSkinJointsAddr() : 0,
				.skinInverseBindsAddr = m_animationDb != nullptr ? m_animationDb->GetSkinInverseBindsAddr() : 0,
				.jobCount = skinJobCount,
			};

			CommandRecorder(cmd).BeginDebugLabel("Animation.BuildSkinPalette", 0.8f, 0.35f, 0.9f, 1.0f);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinCopyPipeline);
			vkCmdPushConstants(cmd, m_skinCopyPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(skinPc), &skinPc);
			const std::uint32_t groups = (skinJobCount + 63u) / 64u;
			vkCmdDispatch(cmd, groups, 1, 1);
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

		{
			AE_PROFILE_ZONE_N("RenderQueue.Cull.Dispatch");
			CommandRecorder(cmd).BeginDebugLabel("CullPass.cullDraws", 0.4f, 0.8f, 0.4f, 1.0f);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
			vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			const std::uint32_t groups = (totalDraws + 63u) / 64u;
			vkCmdDispatch(cmd, groups, 1, 1);
			CommandRecorder(cmd).EndDebugLabel();
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
#endif
	}

	void RenderQueue::FlushDraw(CommandRecorder& recorder, VkDescriptorSet bindlessSet, VkDescriptorSet lightingSet, const GraphicsPipeline* overridePipeline)
	{
		AE_PROFILE_ZONE();
		if (!recorder.IsValid())
		{
			return;
		}
		if (m_batchRenderInfos.empty())
		{
			return;
		}

		recorder.BeginDebugLabel("RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f, 1.0f);

		const DrawPushConstants sharedPc{
			.frameAddr = m_cachedFrameAddr,
			.instanceDataAddr = m_cachedInstanceDataAddr,
			.skinPaletteAddr = m_cachedSkinPaletteAddr,
		};

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
				recorder.BindGraphicsPipeline(*activePipeline);
				lastPipeline = activePipeline;
				lastSetPipeline = nullptr;
				lastLightingSetPipeline = nullptr;
			}

			if (bindlessSet != VK_NULL_HANDLE && activePipeline != nullptr && activePipeline != lastSetPipeline)
			{
				vkCmdBindDescriptorSets(recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline->GetLayout(), 0, 1, &bindlessSet, 0, nullptr);
				lastSetPipeline = activePipeline;
			}

			if (lightingSet != VK_NULL_HANDLE && activePipeline != nullptr && activePipeline->GetSetLayoutCount() > 1 && activePipeline != lastLightingSetPipeline)
			{
				vkCmdBindDescriptorSets(recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, activePipeline->GetLayout(), 1, 1, &lightingSet, 0, nullptr);
				lastLightingSetPipeline = activePipeline;
			}

			if (activePipeline != nullptr)
			{
				recorder.PushConstants(activePipeline->GetLayout(), sharedPc);
			}

			if (batch.mesh != nullptr)
			{
				// Vertex data is fetched via BDA in the vertex shader (DrawInstanceData.vertexBufferAddr).
				// Only the index buffer needs binding to drive SV_VertexID via fixed-function fetch.
				const VkBuffer indexBuffer = batch.mesh->GetIndexBuffer();
				const VkDeviceSize indexOffset = batch.mesh->GetIndexByteOffset();
				if (indexBuffer != lastIndexBuffer || indexOffset != lastIndexOffset)
				{
					recorder.BindIndexBuffer(indexBuffer, indexOffset);
					lastIndexBuffer = indexBuffer;
					lastIndexOffset = indexOffset;
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

	void RenderQueue::Clear(std::uint32_t slot)
	{
		m_commandSlots[slot % kFramesInFlight].clear();
	}

	bool RenderQueue::IsEmpty(std::uint32_t slot) const
	{
		return m_commandSlots[slot % kFramesInFlight].empty();
	}

	void RenderQueue::EnsureSkinCopyPipeline()
	{
		if (m_skinCopyPipeline != VK_NULL_HANDLE)
		{
			return;
		}

		const auto spirv = io::FileSystem::ReadFile("shaders://skin_palette_build.slang.spv");
		if (spirv.empty())
		{
			throw std::runtime_error("RenderQueue: shader not found: shaders://skin_palette_build.slang.spv");
		}

		VkShaderModule shaderModule = vkutil::CreateShaderModule(m_device, spirv, "RenderQueue");

		const VkPushConstantRange pushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(SkinPalettePush),
		};
		const VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_skinCopyPipelineLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			throw std::runtime_error("RenderQueue: failed to create skin copy pipeline layout.");
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
			.layout = m_skinCopyPipelineLayout,
		};
		if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_skinCopyPipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			throw std::runtime_error("RenderQueue: failed to create skin copy compute pipeline.");
		}

		vkDestroyShaderModule(m_device, shaderModule, nullptr);
	}

	void RenderQueue::EnsureAnimationSamplePipeline()
	{
		if (m_animationSamplePipeline != VK_NULL_HANDLE)
		{
			return;
		}

		if (m_animationDb == nullptr || !m_animationDb->IsValid())
		{
			return;
		}

		const auto spirv = io::FileSystem::ReadFile("shaders://animation_sample.slang.spv");
		if (spirv.empty())
		{
			throw std::runtime_error("RenderQueue: shader not found: shaders://animation_sample.slang.spv");
		}

		VkShaderModule shaderModule = vkutil::CreateShaderModule(m_device, spirv, "RenderQueue");

		const VkPushConstantRange pushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(AnimationSamplePush),
		};
		const VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_animationSamplePipelineLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			throw std::runtime_error("RenderQueue: failed to create animation sample pipeline layout.");
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
			.layout = m_animationSamplePipelineLayout,
		};
		if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_animationSamplePipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			throw std::runtime_error("RenderQueue: failed to create animation sample compute pipeline.");
		}

		vkDestroyShaderModule(m_device, shaderModule, nullptr);
	}
} // namespace aether
