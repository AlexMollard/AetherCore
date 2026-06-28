#include "rendering/RenderQueue.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "animation/AnimationBlend.hpp"
#include "io/FileSystem.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "mesh/Mesh.hpp"
#include "utils/Profiler.hpp"
#include "gpu/GpuProfiler.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	void RenderQueue::Initialize(const RenderQueueSharedPipelines& pipelines, const RenderQueueConfig& config)
	{
		m_sharedPipelines = &pipelines;
		m_maxDraws = config.maxDraws;
		m_maxBatches = config.maxBatches;
		m_outputDrawCapacity = (config.outputDrawCapacity > 0) ? config.outputDrawCapacity : config.maxDraws;
		m_maxAnimationDraws = (config.maxAnimationDraws == UINT32_MAX) ? std::min(config.maxDraws, kDefaultMaxAnimationDraws) : config.maxAnimationDraws;
		m_maxSkinJoints = m_maxAnimationDraws * 128u;
		m_maxSampledPoses = m_maxSkinJoints * 2u;
		m_debugName = config.debugName != nullptr ? config.debugName : "RenderQueue";
		m_slotConsumed.fill(true);
		AE_INFO(LogCategory::Render, "RenderQueue::Initialize({}): maxDraws={}, maxAnimationDraws={}, maxSkinJoints={}, maxSampledPoses={}", m_debugName, m_maxDraws, m_maxAnimationDraws, m_maxSkinJoints, m_maxSampledPoses);

		constexpr gpu::BufferUsage kSsboFlags = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress;

		// All buffers route through gpu::ResourceRegistry, which owns the
		// deferred-destruction ring. Per-frame data is mirrored by
		// std::array<Handle, kFramesInFlight> for the hot path.

		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			const gpu::MappedBufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(config.maxDraws) * sizeof(DrawContracts::InstanceData),
			        .usage = kSsboFlags,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = "RenderQueue.InstanceData",
			};
			m_instanceData[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!m_instanceData[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderQueue: InstanceData CreateMappedBuffer failed"));
			}
			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_instanceData[i].handle);
			m_instanceData[i].mapped = view.mappedPtr;
			m_instanceData[i].address = view.deviceAddress;
		}

		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			const gpu::MappedBufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(config.maxDraws) * sizeof(CullContracts::DrawInput),
			        .usage = kSsboFlags,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = "RenderQueue.CullInput",
			};
			m_cullInput[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!m_cullInput[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderQueue: CullInput CreateMappedBuffer failed"));
			}
			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_cullInput[i].handle);
			m_cullInput[i].mapped = view.mappedPtr;
			m_cullInput[i].address = view.deviceAddress;
		}

		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			const gpu::MappedBufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(config.maxBatches) * sizeof(CullContracts::Batch),
			        .usage = kSsboFlags,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = "RenderQueue.BatchDesc",
			};
			m_batchDesc[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!m_batchDesc[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderQueue: BatchDesc CreateMappedBuffer failed"));
			}
			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_batchDesc[i].handle);
			m_batchDesc[i].mapped = view.mappedPtr;
			m_batchDesc[i].address = view.deviceAddress;
		}

		if (m_maxAnimationDraws > 0u)
		{
			for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				const gpu::MappedBufferDesc desc{
				        .size = static_cast<gpu::DeviceSize>(m_maxAnimationDraws) * sizeof(AnimationContracts::SkinCopyJob),
				        .usage = kSsboFlags,
				        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
				        .debugName = "RenderQueue.SkinCopyJobs",
				};
				m_skinCopyJobs[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
				if (!m_skinCopyJobs[i].handle.IsValid())
				{
					Throw(AetherError::Engine("RenderQueue: SkinCopyJobs CreateMappedBuffer failed"));
				}
				const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_skinCopyJobs[i].handle);
				m_skinCopyJobs[i].mapped = view.mappedPtr;
				m_skinCopyJobs[i].address = view.deviceAddress;
			}

			for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				const gpu::MappedBufferDesc desc{
				        .size = static_cast<gpu::DeviceSize>(m_maxAnimationDraws) * sizeof(AnimationContracts::AnimatorSampleJob),
				        .usage = kSsboFlags,
				        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
				        .debugName = "RenderQueue.AnimSampleJobs",
				};
				m_animationSampleJobs[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
				if (!m_animationSampleJobs[i].handle.IsValid())
				{
					Throw(AetherError::Engine("RenderQueue: AnimSampleJobs CreateMappedBuffer failed"));
				}
				const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_animationSampleJobs[i].handle);
				m_animationSampleJobs[i].mapped = view.mappedPtr;
				m_animationSampleJobs[i].address = view.deviceAddress;
			}

			constexpr gpu::BufferUsage kAnimationSsboFlags = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress | gpu::BufferUsage::TransferDst;

			{
				for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
				{
					const gpu::BufferDesc desc{
					        .size = static_cast<gpu::DeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4),
					        .usage = kAnimationSsboFlags,
					        .debugName = "RenderQueue.SkinPalette",
					};
					m_skinPalette[i].handle = gpu::ResourceRegistry::CreateBuffer(desc);
					if (!m_skinPalette[i].handle.IsValid())
					{
						Throw(AetherError::Engine("RenderQueue: SkinPalette CreateBuffer failed"));
					}
					m_skinPalette[i].address = gpu::ResourceRegistry::ResolveBuffer(m_skinPalette[i].handle).deviceAddress;
				}
			}

			for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				const gpu::BufferDesc desc{
				        .size = static_cast<gpu::DeviceSize>(m_maxSampledPoses) * sizeof(AnimationContracts::SampledNodePose),
				        .usage = kAnimationSsboFlags,
				        .debugName = "RenderQueue.SampledPoses",
				};
				m_sampledPoses[i].handle = gpu::ResourceRegistry::CreateBuffer(desc);
				if (!m_sampledPoses[i].handle.IsValid())
				{
					Throw(AetherError::Engine("RenderQueue: SampledPoses CreateBuffer failed"));
				}
				m_sampledPoses[i].address = gpu::ResourceRegistry::ResolveBuffer(m_sampledPoses[i].handle).deviceAddress;
			}

			for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				const gpu::BufferDesc desc{
				        .size = static_cast<gpu::DeviceSize>(m_maxSampledPoses) * sizeof(glm::mat4),
				        .usage = kAnimationSsboFlags,
				        .debugName = "RenderQueue.NodeGlobalTransforms",
				};
				m_nodeGlobalTransforms[i].handle = gpu::ResourceRegistry::CreateBuffer(desc);
				if (!m_nodeGlobalTransforms[i].handle.IsValid())
				{
					Throw(AetherError::Engine("RenderQueue: NodeGlobalTransforms CreateBuffer failed"));
				}
				m_nodeGlobalTransforms[i].address = gpu::ResourceRegistry::ResolveBuffer(m_nodeGlobalTransforms[i].handle).deviceAddress;
			}

			AE_INFO(LogCategory::Render, "RenderQueue animation buffers: skinPalette[0]=0x{:x}, sampledPoses[0]=0x{:x}, nodeGlobalTransforms[0]=0x{:x}", m_skinPalette[0].address, m_sampledPoses[0].address, m_nodeGlobalTransforms[0].address);
		}

		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			const gpu::BufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(m_outputDrawCapacity) * sizeof(gpu::DrawIndexedIndirectCommand),
			        .usage = gpu::BufferUsage::Indirect | gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
			        .debugName = "RenderQueue.IndirectOutput",
			};
			m_outputIndirect[i].handle = gpu::ResourceRegistry::CreateBuffer(desc);
			if (!m_outputIndirect[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderQueue: IndirectOutput CreateBuffer failed"));
			}
			m_outputIndirect[i].address = gpu::ResourceRegistry::ResolveBuffer(m_outputIndirect[i].handle).deviceAddress;
		}

		// Cache the frame-0 mapped pointers for hot-path access (the hot path
		// uses frameIndex % kFramesInFlight to pick the slot).
		m_instanceDataMapped = static_cast<DrawContracts::InstanceData*>(m_instanceData[0].mapped);
		m_cullInputMapped = static_cast<CullContracts::DrawInput*>(m_cullInput[0].mapped);
		m_batchDescMapped = static_cast<CullContracts::Batch*>(m_batchDesc[0].mapped);
		m_skinCopyJobsMapped = static_cast<AnimationContracts::SkinCopyJob*>(m_skinCopyJobs[0].mapped);
		m_animationSampleJobsMapped = static_cast<AnimationContracts::AnimatorSampleJob*>(m_animationSampleJobs[0].mapped);
	}

	void RenderQueue::Shutdown()
	{
		m_sharedPipelines = nullptr;

		auto DestroyAll = [](auto& arr)
		{
			for (auto& e: arr)
			{
				if (e.handle.IsValid())
				{
					gpu::ResourceRegistry::Destroy(e.handle);
				}
				e = {};
			}
		};
		DestroyAll(m_instanceData);
		DestroyAll(m_cullInput);
		DestroyAll(m_batchDesc);
		DestroyAll(m_skinCopyJobs);
		DestroyAll(m_animationSampleJobs);
		DestroyAll(m_outputIndirect);
		DestroyAll(m_sampledPoses);
		DestroyAll(m_nodeGlobalTransforms);
		DestroyAll(m_skinPalette);

		m_instanceDataMapped = nullptr;
		m_cullInputMapped = nullptr;
		m_batchDescMapped = nullptr;
		m_skinCopyJobsMapped = nullptr;
		m_animationSampleJobsMapped = nullptr;

		m_maxDraws = 0;
		m_outputDrawCapacity = 0;
		m_maxBatches = 0;
		m_maxAnimationDraws = 0;
		m_maxSkinJoints = 0;
		m_maxSampledPoses = 0;
		m_animationSlotCleared = {};
	}

	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		std::lock_guard lock(m_slotMutexes[m_writeSlot]);
		m_commandSlots[m_writeSlot].push_back(cmd);
	}

	void RenderQueue::PrepareAndDispatch(gpu::CommandList& cmdList, gpu::DeviceAddress frameAddr, gpu::Pipeline computePipeline, std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		// Keep the raw command buffer for Tracy GPU zones and GpuTimestampPool.
		// The cast to VkCommandBuffer at the Tracy call sites is the one
		// documented allowlist exception (see gpu-abstraction-rendering-audit.md
		// §7.3.0) - Tracy's API requires a raw Vulkan handle.
		const gpu::CommandBuffer rawCmd = cmdList.GetCommandBuffer();
		// Move the commands out of the slot under the lock. This gives the
		// render thread its own copy that the game thread cannot touch.
		// Setting m_slotConsumed and notifying wakes the game thread's Clear()
		// which is waiting for permission to reuse this slot.
		const auto frameSlot = frameIndex % kFramesInFlight;
		std::vector<DrawCommand> commands;
		{
			std::lock_guard lock(m_slotMutexes[frameSlot]);
			commands = std::move(m_commandSlots[frameSlot]);
			m_slotConsumed[frameSlot] = true;
		}
		m_slotCv[frameSlot].notify_one();
		if (commands.empty())
		{
			m_batchRenderInfos.clear();
			return;
		}

		m_cachedFrameAddr = frameAddr;
		m_batchRenderInfos.clear();
		m_animationSampleJobCount = 0;

		const std::uint32_t animJobBase = 0;

		// Update mapped pointers to the current slot's buffers since each slot
		// has its own independent allocation (not one shared mega-buffer).
		m_instanceDataMapped = static_cast<DrawContracts::InstanceData*>(m_instanceData[frameSlot].mapped);
		m_cullInputMapped = static_cast<CullContracts::DrawInput*>(m_cullInput[frameSlot].mapped);
		m_batchDescMapped = static_cast<CullContracts::Batch*>(m_batchDesc[frameSlot].mapped);
		m_skinCopyJobsMapped = static_cast<AnimationContracts::SkinCopyJob*>(m_skinCopyJobs[frameSlot].mapped);
		m_animationSampleJobsMapped = static_cast<AnimationContracts::AnimatorSampleJob*>(m_animationSampleJobs[frameSlot].mapped);

		// Clear the slot's instance data to zero-bounds (w=0 = always visible) so
		// that any entries not overwritten by this frame don't carry stale sphere data
		// from a previous frame that had more draws.
		std::memset(m_instanceDataMapped, 0, m_maxDraws * sizeof(DrawContracts::InstanceData));
		std::memset(m_cullInputMapped, 0, m_maxDraws * sizeof(CullContracts::DrawInput));
		std::memset(m_batchDescMapped, 0, m_maxBatches * sizeof(CullContracts::Batch));
		gpu::ResourceRegistry::FlushMappedBuffer(m_instanceData[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		gpu::ResourceRegistry::FlushMappedBuffer(m_cullInput[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		gpu::ResourceRegistry::FlushMappedBuffer(m_batchDesc[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));

		// Clear device-local animation buffers on first use of each frame slot.
		// Each slot is cleared individually so in-flight slots don't sit with garbage
		// until the first animation sample writes their data.
		if (!m_animationSlotCleared[frameSlot])
		{
			m_animationSlotCleared[frameSlot] = true;
			if (m_skinPalette[frameSlot].handle.IsValid())
			{
				const gpu::DeviceSize slotSize = static_cast<gpu::DeviceSize>(m_maxSkinJoints) * sizeof(glm::mat4);
				cmdList.FillBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_skinPalette[frameSlot].handle), 0, slotSize, 0);
			}
			if (m_sampledPoses[frameSlot].handle.IsValid())
			{
				const gpu::DeviceSize slotSize = static_cast<gpu::DeviceSize>(m_maxSampledPoses) * sizeof(AnimationContracts::SampledNodePose);
				cmdList.FillBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_sampledPoses[frameSlot].handle), 0, slotSize, 0);
			}
			if (m_nodeGlobalTransforms[frameSlot].handle.IsValid())
			{
				const gpu::DeviceSize slotSize = static_cast<gpu::DeviceSize>(m_maxSampledPoses) * sizeof(glm::mat4);
				cmdList.FillBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_nodeGlobalTransforms[frameSlot].handle), 0, slotSize, 0);
			}
			cmdList.PipelineMemoryBarrier(gpu::PipelineStage::Transfer, gpu::AccessFlags::TransferWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);
		}
		m_cachedDrawBase = 0;
		m_cachedIndirectHandle = m_outputIndirect[frameSlot].handle;
		m_cachedInstanceDataAddr = m_instanceData[frameSlot].address;
		const gpu::DeviceAddress currSkinPaletteAddr = (m_maxSkinJoints > 0u) ? m_skinPalette[frameSlot].address : 0;
		const gpu::DeviceAddress currSampledPosesAddr = (m_maxSampledPoses > 0u) ? m_sampledPoses[frameSlot].address : 0;
		const gpu::DeviceAddress currNodeGlobalTransformsAddr = (m_maxSampledPoses > 0u) ? m_nodeGlobalTransforms[frameSlot].address : 0;
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

		std::ranges::stable_sort(commands,

		        [](const DrawCommand& a, const DrawCommand& b)
		        {
			        if (a.pipeline != b.pipeline)
			        {
				        return a.pipeline < b.pipeline;
			        }
			        return a.mesh < b.mesh;
		        });

		const auto totalDraws = static_cast<std::uint32_t>(commands.size());
		AE_ASSERT_ALWAYS(totalDraws <= m_maxDraws, "RenderQueue: exceeded maxDraws - increase Initialize capacity.");

		std::uint32_t globalDrawIdx = 0; // monotonically increasing index within this frame slot
		std::uint32_t batchIdx = 0;

		for (std::size_t i = 0; i < commands.size();)
		{
			const GraphicsPipeline* batchPipeline = commands[i].pipeline;
			const Mesh* batchMesh = commands[i].mesh;

			std::size_t batchEnd = i;
			while (batchEnd < commands.size() && commands[batchEnd].pipeline == batchPipeline && commands[batchEnd].mesh == batchMesh)
			{
				++batchEnd;
			}

			if (batchMesh == nullptr || !batchMesh->IsAlive() || !batchMesh->IsValid() || !batchMesh->GetIndexBuffer().IsValid())
			{
				i = batchEnd;
				continue;
			}

			const std::uint32_t batchOutputStart = globalDrawIdx;
			const auto batchDrawCount = static_cast<std::uint32_t>(batchEnd - i);

			AE_ASSERT_ALWAYS(batchIdx < m_maxBatches, "RenderQueue: exceeded maxBatches - increase Initialize capacity.");

			for (std::size_t j = i; j < batchEnd; ++j)
			{
				const DrawCommand& dc = commands[j];

				if (dc.mesh && (!dc.mesh->IsAlive() || !dc.mesh->IsValid() || dc.mesh->GetGeneration() != dc.meshGeneration || !dc.mesh->GetIndexBuffer().IsValid()))
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

				const bool wantsGpuSampling = gpuSamplingEnabled && dbValid && dc.skinJointCount > 0 && dc.skinIndex >= 0 && dc.animClipIndex < drawClipCount && std::cmp_less(dc.skinIndex, drawSkinCount);
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
						        .sampledPosesAddr = currSampledPosesAddr + static_cast<gpu::DeviceSize>(nodePoseCursor) * sizeof(AnimationContracts::SampledNodePose),
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
							animSampleBatches[animSampleBatchCount++] = {.db = drawAnimDb, .dbGeneration = drawAnimDb ? drawAnimDb->GetGeneration() : 0, .startJob = sampleJobsThisFrame, .count = 1u};
						}

						if (skinPaletteBatchCount > 0 && skinPaletteBatches[skinPaletteBatchCount - 1].db == drawAnimDb)
						{
							skinPaletteBatches[skinPaletteBatchCount - 1].count++;
						}
						else
						{
							AE_ASSERT_ALWAYS(skinPaletteBatchCount < 64, "RenderQueue: too many skin palette batches");
							skinPaletteBatches[skinPaletteBatchCount++] = {.db = drawAnimDb, .dbGeneration = drawAnimDb ? drawAnimDb->GetGeneration() : 0, .startJob = skinJobCount, .count = 1u};
						}

						++sampleJobsThisFrame;
						++skinJobCount;
						skinJointCursor += dc.skinJointCount;
						nodePoseCursor += drawNodeCount;
					}
				}

				m_instanceDataMapped[globalDrawIdx] = DrawContracts::InstanceData{
				        .model = dc.modelMatrix,
				        .materialIndex = dc.materialIndex,
				        .skinPaletteOffset = skinPaletteOffset,
				        .skinJointCount = skinJointCount,
				        .worldBoundingSphere = dc.worldBoundingSphere,
				        .vertexBufferAddr = (dc.mesh != nullptr) ? dc.mesh->GetVertexDeviceAddress() : 0,
				};

				m_cullInputMapped[globalDrawIdx] = CullContracts::DrawInput{
				        .indexCount = (dc.mesh != nullptr) ? dc.mesh->GetIndexCount() : 0u,
				        .instanceCount = 1u,
				        .firstIndex = 0u,
				        .vertexOffset = 0,
				        .firstInstance = globalDrawIdx, // frame-relative; BDA base (m_cachedInstanceDataAddr) accounts for slot
				        .batchIndex = batchIdx,
				};

				++globalDrawIdx;
			}

			m_batchDescMapped[batchIdx] = CullContracts::Batch{.inputStart = batchOutputStart, .drawCount = batchDrawCount, .outputStart = batchOutputStart};

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
		gpu::ResourceRegistry::FlushMappedBuffer(m_instanceData[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		gpu::ResourceRegistry::FlushMappedBuffer(m_cullInput[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		gpu::ResourceRegistry::FlushMappedBuffer(m_batchDesc[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		if (m_skinCopyJobsMapped != nullptr)
		{
			gpu::ResourceRegistry::FlushMappedBuffer(m_skinCopyJobs[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		}
		if (sampleJobsThisFrame > 0)
		{
			gpu::ResourceRegistry::FlushMappedBuffer(m_animationSampleJobs[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(-1));
		}

		// Ensure host writes are visible to subsequent shader reads.
		cmdList.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::AllCommands, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

		if (sampleJobsThisFrame > 0 && !m_debugDisableAnimation)
		{
			AE_VERBOSE(LogCategory::Animation, "Animation dispatch enabled: {} sampleJobs, {} skinJobs", sampleJobsThisFrame, skinJobCount);
			// -- Pass 0: Parallel bind-pose initialization --
			// Dispatched before animation sampling to write all node bind poses
			// in parallel (each thread handles one (job, node) pair).
			if ((m_debugAnimPassMask & 1u) && m_sharedPipelines != nullptr && m_sharedPipelines->poseInit.IsValid())
			{
				const auto poseInitPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->poseInit);
				AE_PROFILE_ZONE();
				AE_VERBOSE(LogCategory::Animation, "PoseInit: currSampledPosesAddr=0x{:x}, animJobsBDA=0x{:x}", currSampledPosesAddr, m_animationSampleJobs[frameSlot].address);

				cmdList.BindComputePipeline(const_cast<void*>(poseInitPipe.state));
				cmdList.BeginDebugLabel("Animation.PoseInit", 0.9f, 0.6f, 0.3f, 1.0f);

				const gpu::DeviceAddress animJobsBDAForInit = m_animationSampleJobs[frameSlot].address;

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
					        .animatorJobsAddr = animJobsBDAForInit + static_cast<gpu::DeviceSize>(batch.startJob) * sizeof(AnimationContracts::AnimatorSampleJob),
					        .sampledPosesAddr = currSampledPosesAddr,
					        .jobCount = batch.count,
					        .nodeCountPerJob = nodeCount,
					};
					cmdList.PushDataRaw(0, std::span(reinterpret_cast<const std::byte*>(&initPc), sizeof(initPc)));

					const std::uint32_t groupsX = (batch.count + 7u) / 8u;
					const std::uint32_t groupsY = (nodeCount + 7u) / 8u;
					cmdList.Dispatch(groupsX, groupsY, 1);
				}

				cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

				cmdList.EndDebugLabel();
			}

			AE_PROFILE_ZONE();

			if ((m_debugAnimPassMask & 2u) && m_sharedPipelines != nullptr && m_sharedPipelines->animSample.IsValid() && sampleJobsThisFrame > 0)
			{
				const auto animSamplePipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->animSample);
				cmdList.BindComputePipeline(const_cast<void*>(animSamplePipe.state));
				cmdList.BeginDebugLabel("Animation.SampleClips", 0.9f, 0.6f, 0.3f, 1.0f);

				const AnimationContracts::AnimationSamplePush animPc{
				        .animDbClipsAddr = 0,
				        .animDbChannelsAddr = 0,
				        .animDbTimesAddr = 0,
				        .animDbValuesAddr = 0,
				        .bindTranslationsAddr = 0,
				        .bindRotationsAddr = 0,
				        .bindScalesAddr = 0,
				        .animatorJobsAddr = m_animationSampleJobs[frameSlot].address,
				        .sampledPosesAddr = currSampledPosesAddr,
				        .jobCount = sampleJobsThisFrame,
				        .clipCount = 0,
				};
				cmdList.PushDataRaw(0, std::span(reinterpret_cast<const std::byte*>(&animPc), sizeof(animPc)));
				{
					AE_GPU_ZONE_SCOPED(rawCmd, "Animation.SampleClips");
					const std::uint32_t groups = (sampleJobsThisFrame + 63u) / 64u;
					cmdList.Dispatch(groups, 1, 1);
				}

				cmdList.EndDebugLabel();

				// Barrier: make GPU anim_sample writes visible to downstream
				// compute passes (node_flatten, build_skin_palette).
				cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead);
			}

			// -- Pass 1.3: Animation blend (cross-fade between two clips) -----------
			if (m_animationBlendSystem != nullptr && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && (m_debugAnimPassMask & 2u))
			{
				const AnimationContracts::AnimationBlendPush& blendPc = m_animationBlendSystem->GetBlendPush();
				const std::uint32_t blendJobCount = m_animationBlendSystem->GetBlendJobCount();
				const bool blendPushValid = blendPc.blendJobsAddr != 0 && blendPc.sampledPosesAddr != 0;
				if (blendJobCount > 0 && blendPc.jobCount > 0 && blendPushValid)
				{
					AE_PROFILE_ZONE();
					const auto animBlendPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->animBlend);
					cmdList.BindComputePipeline(const_cast<void*>(animBlendPipe.state));
					cmdList.BeginDebugLabel("Animation.AnimBlend", 0.6f, 0.4f, 0.8f, 1.0f);
					cmdList.PushDataRaw(0, std::span<const std::byte>(reinterpret_cast<const std::byte*>(&blendPc), sizeof(blendPc)));
					{
						AE_GPU_ZONE_SCOPED(rawCmd, "Animation.AnimBlend");
						const std::uint32_t groups = (blendPc.jobCount + 63u) / 64u;
						cmdList.Dispatch(groups, 1, 1);
					}
					cmdList.EndDebugLabel();

					cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead);
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

		// -- Pass 1.5: Flatten per-node global transforms (level-by-level depth dispatch) --
		if ((m_debugAnimPassMask & 4u) && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && m_sharedPipelines->nodeFlatten.IsValid())
		{
			const auto nodeFlattenPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->nodeFlatten);
			AE_PROFILE_ZONE();
			AE_VERBOSE(LogCategory::Animation, "NodeFlatten: {} batches, {} sampleJobs", animSampleBatchCount, sampleJobsThisFrame);

			cmdList.BindComputePipeline(const_cast<void*>(nodeFlattenPipe.state));
			cmdList.BeginDebugLabel("Animation.NodeFlatten", 0.3f, 0.8f, 0.6f, 1.0f);

			const gpu::DeviceAddress animJobsBDA = m_animationSampleJobs[frameSlot].address;

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
				const std::uint32_t depthCount = batch.db->GetDepthCount();
				AE_VERBOSE(LogCategory::Animation, "  Batch[{}]: depthCount={} nodeParentsAddr=0x{:x} depthSortedNodesAddr=0x{:x}", bi, depthCount, batch.db->GetNodeParentsAddr(), batch.db->GetDepthSortedNodesAddr());
				if (depthCount == 0)
				{
					continue;
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
					cmdList.PushDataRaw(0, std::span<const std::byte>(reinterpret_cast<const std::byte*>(&flattenPc), sizeof(flattenPc)));

					const std::uint32_t totalWork = batch.count * range.count;
					const std::uint32_t groups = (totalWork + 63u) / 64u;
					cmdList.Dispatch(groups, 1, 1);

					// Barrier between depth levels: parent writes from this
					// dispatch must be visible to the next level's reads.
					if (di + 1 < depthCount)
					{
						cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead);
					}
				}
			}
			cmdList.EndDebugLabel();

			cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead);
		}

		if ((m_debugAnimPassMask & 8u) && skinJobCount > 0 && !m_debugDisableAnimation)
		{
			AE_PROFILE_ZONE();

			const auto skinPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->skinCopy);
			cmdList.BindComputePipeline(const_cast<void*>(skinPipe.state));
			cmdList.BeginDebugLabel("Animation.BuildSkinPalette", 0.8f, 0.35f, 0.9f, 1.0f);

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
				        .jobsAddr = m_skinCopyJobs[frameSlot].address + static_cast<gpu::DeviceSize>(batch.startJob) * sizeof(AnimationContracts::SkinCopyJob),
				        .dstPaletteAddr = currSkinPaletteAddr,
				        .globalTransformsAddr = currNodeGlobalTransformsAddr,
				        .skinMetasAddr = batch.db->GetSkinMetasAddr(),
				        .skinJointsAddr = batch.db->GetSkinJointsAddr(),
				        .skinInverseBindsAddr = batch.db->GetSkinInverseBindsAddr(),
				        .jobCount = batch.count,
				};
				cmdList.PushDataRaw(0, std::span<const std::byte>(reinterpret_cast<const std::byte*>(&skinPc), sizeof(skinPc)));
				{
					AE_GPU_ZONE_SCOPED(rawCmd, "Animation.BuildSkinPalette");
					const std::uint32_t groups = (batch.count + 63u) / 64u;
					cmdList.Dispatch(groups, 1, 1);
				}
			}

			cmdList.EndDebugLabel();

			cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::AllCommands, gpu::AccessFlags::ShaderStorageRead);
		}
		else if (sampleJobsThisFrame > 0 && m_debugDisableAnimation)
		{
			AE_WARN(LogCategory::Animation, "Animation dispatch DISABLED by debug flag ({} sampleJobs, {} skinJobs skipped)", sampleJobsThisFrame, skinJobCount);
		}

		// -- Cull dispatch: single or multi-frustum --
		const gpu::DeviceSize inputCmdOffset = 0;
		const gpu::DeviceSize batchDescOffset = 0;

		if (m_outputDrawCapacity > m_maxDraws)
		{
			// Multi-frustum mode (shadow cascades): test each draw against 3 VP matrices,
			// write 3 independent output regions.  The 3 frame constant BDAs must have
			// been set via SetMultiCullFrameAddrs() before this call.
			const gpu::DeviceSize outputCmdOffset = 0;
			const gpu::DeviceSize cascadeStride = static_cast<gpu::DeviceSize>(m_maxDraws) * sizeof(gpu::DrawIndexedIndirectCommand);

			const CullContracts::MultiPushConstants multiPc{
			        .frameAddrs = {m_multiFrameAddrs[0], m_multiFrameAddrs[1], m_multiFrameAddrs[2]},
			        .instanceDataAddr = m_cachedInstanceDataAddr,
			        .inputCmdAddr = m_cullInput[frameSlot].address + inputCmdOffset,
			        .outputCmdAddr = m_outputIndirect[frameSlot].address + outputCmdOffset,
			        .batchDescAddr = m_batchDesc[frameSlot].address + batchDescOffset,
			        .totalDrawCount = totalDraws,
			        .outputCascadeStride = static_cast<std::uint32_t>(cascadeStride),
			        .debugFlags = m_debugForceVisible ? CullContracts::kDebugForceVisibleBit : 0u,
			};

			{
				AE_PROFILE_ZONE();
				cmdList.BeginDebugLabel("CullPass.cullDrawsMulti", 0.4f, 0.8f, 0.4f, 1.0f);
				cmdList.BindComputePipeline(computePipeline);
				cmdList.PushDataRaw(0, std::span(reinterpret_cast<const std::byte*>(&multiPc), sizeof(multiPc)));
				const std::uint32_t groups = (totalDraws + 63u) / 64u;
				cmdList.Dispatch(groups, 1, 1);
				cmdList.EndDebugLabel();
			}
		}
		else
		{
			// Single-frustum mode (main camera, local shadows).
			const gpu::DeviceSize outputCmdOffset = 0;
			const CullContracts::PushConstants pc{
			        .frameAddr = frameAddr,
			        .instanceDataAddr = m_cachedInstanceDataAddr,
			        .inputCmdAddr = m_cullInput[frameSlot].address + inputCmdOffset,
			        .outputCmdAddr = m_outputIndirect[frameSlot].address + outputCmdOffset,
			        .batchDescAddr = m_batchDesc[frameSlot].address + batchDescOffset,
			        .batchCountAddr = 0,
			        .totalDrawCount = totalDraws,
			        .debugFlags = m_debugForceVisible ? CullContracts::kDebugForceVisibleBit : 0u,
			};

			{
				AE_PROFILE_ZONE();
				cmdList.BeginDebugLabel("CullPass.cullDraws", 0.4f, 0.8f, 0.4f, 1.0f);
				cmdList.BindComputePipeline(computePipeline);
				cmdList.PushDataRaw(0, std::span(reinterpret_cast<const std::byte*>(&pc), sizeof(pc)));
				{
					AE_GPU_ZONE_SCOPED(rawCmd, "CullPass.cullDraws");
					const std::uint32_t groups = (totalDraws + 63u) / 64u;
					cmdList.Dispatch(groups, 1, 1);
				}
				cmdList.EndDebugLabel();
			}
		}

		// Ensure indirect args are visible before draw-indirect.
		cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::AllCommands, gpu::AccessFlags::IndirectCommandRead);

#ifdef TRACY_ENABLE
		{
			AE_PROFILE_PLOT("Animation/SampleJobs", static_cast<int64_t>(sampleJobsThisFrame));
			AE_PROFILE_PLOT("Animation/SkinCopyJobs", static_cast<int64_t>(skinJobCount));
			AE_PROFILE_PLOT("RenderQueue/TotalDraws", static_cast<int64_t>(totalDraws));
		}
#endif
	}

	void RenderQueue::FlushDraw(gpu::CommandList& cmd, const DrawContracts::LightingAddresses* lighting, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset)
	{
		FlushDrawImpl(cmd, m_cachedFrameAddr, lighting, overridePipeline, cascadeOffset, "RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f);
	}

	void RenderQueue::FlushDrawPush(gpu::CommandList& cmd, const DrawContracts::LightingAddresses& lighting, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset)
	{
		FlushDrawImpl(cmd, m_cachedFrameAddr, &lighting, overridePipeline, cascadeOffset, "RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f);
	}

	void RenderQueue::FlushDrawWithFrameAddr(gpu::CommandList& cmd, const DrawContracts::LightingAddresses* lighting, const gpu::DeviceAddress overrideFrameAddr, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset)
	{
		FlushDrawImpl(cmd, overrideFrameAddr, lighting, overridePipeline, cascadeOffset, "RenderQueue.FlushDrawWithAddr", 0.85f, 0.40f, 0.60f);
	}

	void RenderQueue::FlushDrawImpl(
	        gpu::CommandList& cmd, gpu::DeviceAddress frameAddr, const DrawContracts::LightingAddresses* lighting, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset, const char* debugLabel, float r, float g, float b)
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
		        .lightDataAddr = lighting ? lighting->lightDataAddr : gpu::DeviceAddress{0},
		        .tileHeadersAddr = lighting ? lighting->tileHeadersAddr : gpu::DeviceAddress{0},
		        .tileLightIndicesAddr = lighting ? lighting->tileLightIndicesAddr : gpu::DeviceAddress{0},
		};
		AE_VERBOSE(LogCategory::Render, "FlushDraw: frameAddr=0x{:x}, instanceDataAddr=0x{:x}, skinPaletteAddr=0x{:x}, batches={}", frameAddr, m_cachedInstanceDataAddr, m_cachedSkinPaletteAddr, m_batchRenderInfos.size());

		const GraphicsPipeline* lastPipeline = nullptr;
		gpu::BufferHandle lastIndexBuffer{};
		gpu::DeviceSize lastIndexOffset = ~0ull;
		const GraphicsPipeline* lastSetPipeline = nullptr;

		for (const auto& batch: m_batchRenderInfos)
		{
			const GraphicsPipeline* activePipeline = overridePipeline != nullptr ? overridePipeline : batch.pipeline;

			if (activePipeline != nullptr && activePipeline != lastPipeline)
			{
				cmd.BindPipeline(activePipeline->GetPipeline());
				lastPipeline = activePipeline;
				lastSetPipeline = nullptr;
			}

			if (activePipeline != nullptr)
			{
				cmd.PushDataRaw(0, gpu::AsPushConstantBytes(sharedPc));
			}

			if (batch.mesh != nullptr && batch.mesh->IsAlive() && batch.mesh->IsValid() && batch.mesh->GetGeneration() == batch.meshGeneration && batch.mesh->GetIndexBuffer().IsValid())
			{
				const gpu::BufferHandle indexBufferHandle = batch.mesh->GetIndexBuffer();
				const gpu::DeviceSize indexOffset = batch.mesh->GetIndexByteOffset();
				if (indexBufferHandle != lastIndexBuffer || indexOffset != lastIndexOffset)
				{
					cmd.BindIndexBuffer(indexBufferHandle, indexOffset);
					lastIndexBuffer = indexBufferHandle;
					lastIndexOffset = indexOffset;
				}
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
				cmd.DrawIndexedIndirect(gpu::ResourceRegistry::ResolveBufferVkHandle(m_cachedIndirectHandle),
				        static_cast<gpu::DeviceSize>(m_cachedDrawBase + cascadeOffset + batch.outputStart) * sizeof(gpu::DrawIndexedIndirectCommand),
				        batch.drawCount,
				        sizeof(gpu::DrawIndexedIndirectCommand));
			}
		}

		cmd.EndDebugLabel();
	}

	void RenderQueue::Clear(std::uint32_t slot)
	{
		const auto idx = slot % kFramesInFlight;
		std::unique_lock lock(m_slotMutexes[idx]);
		while (!m_slotConsumed[idx])
		{
			if (m_slotCv[idx].wait_for(lock, std::chrono::seconds(2), [this, idx] { return m_slotConsumed[idx]; }))
			{
				break;
			}
			AE_WARN(LogCategory::Render, "RenderQueue::Clear({}): waiting for '{}' slot {} to be consumed. The cull pass for this queue may have been skipped or disabled.", slot, m_debugName, idx);
		}
		m_slotConsumed[idx] = false;
		m_commandSlots[idx].clear();
	}

	void RenderQueue::DiscardPending(std::uint32_t slot)
	{
		const auto idx = slot % kFramesInFlight;
		{
			std::lock_guard lock(m_slotMutexes[idx]);
			m_commandSlots[idx].clear();
			m_slotConsumed[idx] = true;
		}
		m_slotCv[idx].notify_one();
	}

	void RenderQueue::DiscardAllPending()
	{
		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			DiscardPending(i);
		}
	}

	bool RenderQueue::IsEmpty(std::uint32_t slot) const
	{
		return m_commandSlots[slot % kFramesInFlight].empty();
	}

	void RenderQueueSharedPipelines::Initialize(gpu::Device device)
	{
		skinCopy = gpu::ResourceRegistry::CreateComputePipeline(device,
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://skin_palette_build.spv",
		                .shaderEntry = "main",
		                .debugName = "Animation.BuildSkinPalette",
		        });
		if (!skinCopy.IsValid())
		{
			Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create skin copy compute pipeline."));
		}

		animSample = gpu::ResourceRegistry::CreateComputePipeline(device,
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://animation_sample.spv",
		                .shaderEntry = "main",
		                .debugName = "Animation.SampleClips",
		        });
		if (!animSample.IsValid())
		{
			Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create animation sample compute pipeline."));
		}

		poseInit = gpu::ResourceRegistry::CreateComputePipeline(device,
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://pose_init.spv",
		                .shaderEntry = "main",
		                .debugName = "Animation.PoseInit",
		        });
		if (!poseInit.IsValid())
		{
			Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create pose init compute pipeline."));
		}

		nodeFlatten = gpu::ResourceRegistry::CreateComputePipeline(device,
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://node_flatten.spv",
		                .shaderEntry = "main",
		                .debugName = "Animation.NodeFlatten",
		        });
		if (!nodeFlatten.IsValid())
		{
			Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create nodeFlatten compute pipeline."));
		}

		animBlend = gpu::ResourceRegistry::CreateComputePipeline(device,
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://anim_blend.spv",
		                .shaderEntry = "main",
		                .debugName = "Animation.AnimBlend",
		        });
		if (!animBlend.IsValid())
		{
			Throw(AetherError::Vulkan(0, "RenderQueueSharedPipelines: failed to create animBlend compute pipeline."));
		}
	}

	void RenderQueueSharedPipelines::Shutdown()
	{
		if (skinCopy.IsValid())
		{
			gpu::ResourceRegistry::Destroy(skinCopy);
			skinCopy = {};
		}
		if (animSample.IsValid())
		{
			gpu::ResourceRegistry::Destroy(animSample);
			animSample = {};
		}
		if (nodeFlatten.IsValid())
		{
			gpu::ResourceRegistry::Destroy(nodeFlatten);
			nodeFlatten = {};
		}
		if (poseInit.IsValid())
		{
			gpu::ResourceRegistry::Destroy(poseInit);
			poseInit = {};
		}
		if (animBlend.IsValid())
		{
			gpu::ResourceRegistry::Destroy(animBlend);
			animBlend = {};
		}
	}
} // namespace aether
