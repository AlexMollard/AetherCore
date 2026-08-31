#include "rendering/RenderQueue.hpp"

#include <bit>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
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


		for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			const gpu::BufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(m_outputDrawCapacity) * sizeof(gpu::DrawIndexedIndirectCommand),
			        .usage = gpu::BufferUsage::Indirect | gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress | gpu::BufferUsage::TransferDst,
			        .debugName = "RenderQueue.IndirectOutput",
			};
			m_outputIndirect[i].handle = gpu::ResourceRegistry::CreateBuffer(desc);
			if (!m_outputIndirect[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderQueue: IndirectOutput CreateBuffer failed"));
			}
			m_outputIndirect[i].address = gpu::ResourceRegistry::ResolveBuffer(m_outputIndirect[i].handle).deviceAddress;
		}

		m_instanceDataMapped = static_cast<DrawContracts::InstanceData*>(m_instanceData[0].mapped);
		m_cullInputMapped = static_cast<CullContracts::DrawInput*>(m_cullInput[0].mapped);
		m_batchDescMapped = static_cast<CullContracts::Batch*>(m_batchDesc[0].mapped);
	}

	// The skinning buffers are sized for the queue's worst case (maxAnimationDraws
	// skeletons of 128 joints, triple-buffered) and that reservation costs over a
	// hundred megabytes per queue. A scene with no skinned meshes - every 2D scene,
	// and most 3D ones - would never touch a byte of it, so the buffers are created
	// the first time this queue is actually handed an animated draw and kept from
	// then on. Capacity is unchanged; only the moment of allocation moved.
	void RenderQueue::EnsureAnimationBuffers()
	{
		AE_PROFILE_ZONE();
		if (m_animationBuffersReady || m_maxAnimationDraws == 0u)
		{
			return;
		}

		constexpr gpu::BufferUsage kSsboFlags = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress;
		constexpr gpu::BufferUsage kAnimationSsboFlags = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress | gpu::BufferUsage::TransferDst;

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

		// Freshly created buffers hold garbage, so every slot needs its zero-fill
		// again before the animation compute passes read from it.
		m_animationSlotCleared.fill(false);
		m_animationInputHash.fill(0ull);
		m_animationBuffersReady = true;

		AE_INFO(LogCategory::Render,
		        "RenderQueue({}): animation buffers created on first skinned draw: skinPalette[0]=0x{:x}, sampledPoses[0]=0x{:x}, nodeGlobalTransforms[0]=0x{:x}",
		        m_debugName,
		        m_skinPalette[0].address,
		        m_sampledPoses[0].address,
		        m_nodeGlobalTransforms[0].address);
	}

	// The mirror of EnsureAnimationBuffers. Everything it touches is put back exactly as
	// this queue was before its first skinned draw: null handles, null mapped pointers,
	// zero device addresses. PrepareAndDispatch already treats that state as "no GPU
	// skinning this frame" - it is the state every queue starts life in - so releasing
	// cannot make a frame render wrong, only make the next skinned frame re-create.
	//
	// Capacity (m_maxAnimationDraws and friends) is deliberately left alone: it is the
	// queue's configuration, not its allocation, and EnsureAnimationBuffers reads it.
	void RenderQueue::ReleaseAnimationBuffers()
	{
		AE_PROFILE_ZONE();
		if (!m_animationBuffersReady)
		{
			return;
		}

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
		DestroyAll(m_skinCopyJobs);
		DestroyAll(m_animationSampleJobs);
		DestroyAll(m_sampledPoses);
		DestroyAll(m_nodeGlobalTransforms);
		DestroyAll(m_skinPalette);

		m_skinCopyJobsMapped = nullptr;
		m_animationSampleJobsMapped = nullptr;
		// The addresses these frames were prepared against are gone; a stale one handed to
		// a shader would be a dangling device pointer.
		for (PreparedFrame& prepared: m_preparedFrames)
		{
			prepared.skinPaletteAddr = 0;
			prepared.nodeGlobalTransformsAddr = 0;
		}
		m_animationSlotCleared.fill(false);
		// The palettes those hashes described are gone.
		m_animationInputHash.fill(0ull);
		m_animationBuffersReady = false;

		AE_INFO(LogCategory::Render, "RenderQueue({}): animation buffers released after sustained absence of skinned draws.", m_debugName);
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
		for (PreparedFrame& prepared: m_preparedFrames)
		{
			prepared.Reset();
		}

		m_maxDraws = 0;
		m_outputDrawCapacity = 0;
		m_maxBatches = 0;
		m_maxAnimationDraws = 0;
		m_maxSkinJoints = 0;
		m_maxSampledPoses = 0;
		m_animationSlotCleared = {};
		m_animationInputHash = {};
		m_animationBuffersReady = false;
		m_slotHasAnimatedDraws = {};
	}

	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		const std::lock_guard lock(m_slotMutexes[m_writeSlot]);
		m_commandSlots[m_writeSlot].push_back(cmd);
		// Exactly the condition EnsureAnimationBuffers tests on the render thread, recorded
		// here so the game thread can answer "does this frame need the skinning pools?"
		// without walking the command list a second time.
		if (cmd.animDb != nullptr)
		{
			m_slotHasAnimatedDraws[m_writeSlot] = true;
		}
	}

	void RenderQueue::PrepareAndDispatch(gpu::CommandList& cmdList, gpu::DeviceAddress frameAddr, gpu::PipelineView computePipeline, std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		const gpu::CommandBuffer rawCmd = cmdList.GetCommandBuffer();
		// Move the commands out of the slot under the lock. This gives the
		const auto frameSlot = frameIndex % kFramesInFlight;
		std::vector<DrawCommand> commands;
		{
			const std::lock_guard lock(m_slotMutexes[frameSlot]);
			commands = std::move(m_commandSlots[frameSlot]);
			m_slotConsumed[frameSlot] = true;
		}
		m_slotCv[frameSlot].notify_one();
		PreparedFrame& prepared = m_preparedFrames[frameSlot];
		prepared.Reset();
		if (commands.empty())
		{
			return;
		}

		prepared.frameAddr = frameAddr;
		m_animationSampleJobCount = 0;

		if (!m_animationBuffersReady && std::ranges::any_of(commands, [](const DrawCommand& dc) { return dc.animDb != nullptr; }))
		{
			EnsureAnimationBuffers();
		}

		const std::uint32_t animJobBase = 0;

		m_instanceDataMapped = static_cast<DrawContracts::InstanceData*>(m_instanceData[frameSlot].mapped);
		m_cullInputMapped = static_cast<CullContracts::DrawInput*>(m_cullInput[frameSlot].mapped);
		m_batchDescMapped = static_cast<CullContracts::Batch*>(m_batchDesc[frameSlot].mapped);
		m_skinCopyJobsMapped = static_cast<AnimationContracts::SkinCopyJob*>(m_skinCopyJobs[frameSlot].mapped);
		m_animationSampleJobsMapped = static_cast<AnimationContracts::AnimatorSampleJob*>(m_animationSampleJobs[frameSlot].mapped);

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
		prepared.drawBase = 0;
		prepared.indirectHandle = m_outputIndirect[frameSlot].handle;
		prepared.instanceDataAddr = m_instanceData[frameSlot].address;
		const gpu::DeviceAddress currSkinPaletteAddr = (m_maxSkinJoints > 0u) ? m_skinPalette[frameSlot].address : 0;
		const gpu::DeviceAddress currSampledPosesAddr = (m_maxSampledPoses > 0u) ? m_sampledPoses[frameSlot].address : 0;
		const gpu::DeviceAddress currNodeGlobalTransformsAddr = (m_maxSampledPoses > 0u) ? m_nodeGlobalTransforms[frameSlot].address : 0;
		prepared.nodeGlobalTransformsAddr = currNodeGlobalTransformsAddr;
		prepared.skinPaletteAddr = currSkinPaletteAddr;
		const bool gpuSamplingEnabled = m_animationSampleJobsMapped != nullptr && m_skinCopyJobsMapped != nullptr && m_maxAnimationDraws > 0u;

		std::uint32_t skinJointCursor = 0;
		std::uint32_t nodePoseCursor = 0;
		std::uint64_t animationInputHash = 0ull;
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
			        // Transparent geometry draws after everything opaque. It does not write
			        // depth, so anything opaque submitted after it would pass the depth test
			        // and paint straight over it - the blend would be undone by the very
			        // surface it was meant to show through.
			        if (a.blended != b.blended)
			        {
				        return !a.blended;
			        }
			        // Transparent surfaces composite in the order they are drawn, so two of
			        // them overlapping only reads correctly if the far one goes down first.
			        // Opaque geometry does not need this - the depth test resolves it however
			        // it is ordered - and sorting it by depth would break the batching below,
			        // so only the blended group pays for it.
			        if (a.blended && a.viewDepthSq != b.viewDepthSq)
			        {
				        return a.viewDepthSq > b.viewDepthSq;
			        }
			        // Within each group the order stays by pipeline then mesh, which is what
			        // keeps state changes down.
			        if (a.pipeline != b.pipeline)
			        {
				        return a.pipeline < b.pipeline;
			        }
			        return a.mesh < b.mesh;
		        });

		const auto submittedDraws = static_cast<std::uint32_t>(commands.size());
		AE_ASSERT_ALWAYS(submittedDraws <= m_maxDraws, "RenderQueue: exceeded maxDraws - increase Initialize capacity.");

		std::uint32_t globalDrawIdx = 0;
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

			AE_ASSERT_ALWAYS(batchIdx < m_maxBatches, "RenderQueue: exceeded maxBatches - increase Initialize capacity.");

			for (std::size_t j = i; j < batchEnd; ++j)
			{
				const DrawCommand& dc = commands[j];

				if ((dc.mesh != nullptr) && (!dc.mesh->IsAlive() || !dc.mesh->IsValid() || dc.mesh->GetGeneration() != dc.meshGeneration || !dc.mesh->GetIndexBuffer().IsValid()))
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

						// Everything the palette content depends on. Offsets are in here too,
						// so a reordered draw list counts as a change even when the poses
						// themselves are identical.
						const auto mix = [&animationInputHash](std::uint64_t v)
						{
							animationInputHash ^= v + 0x9e3779b97f4a7c15ull + (animationInputHash << 6) + (animationInputHash >> 2);
						};
						mix(reinterpret_cast<std::uintptr_t>(drawAnimDb));
						mix(drawAnimDb->GetGeneration());
						mix(static_cast<std::uint64_t>(dc.animClipIndex));
						mix(std::bit_cast<std::uint32_t>(dc.animTime));
						mix(static_cast<std::uint64_t>(dc.skinIndex));
						mix((static_cast<std::uint64_t>(skinPaletteOffset) << 32) | dc.skinJointCount);
						mix((static_cast<std::uint64_t>(nodePoseCursor) << 32) | drawNodeCount);

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
				        .effectParamIndex = dc.effectParamIndex,
				        .worldBoundingSphere = dc.worldBoundingSphere,
				        .vertexBufferAddr = (dc.mesh != nullptr) ? dc.mesh->GetVertexDeviceAddress() : 0,
				};

				m_cullInputMapped[globalDrawIdx] = CullContracts::DrawInput{
				        .indexCount = (dc.mesh != nullptr) ? dc.mesh->GetIndexCount() : 0u,
				        .instanceCount = 1u,
				        .firstIndex = 0u,
				        .vertexOffset = 0,
				        .firstInstance = globalDrawIdx,
				        .batchIndex = batchIdx,
				};

				++globalDrawIdx;
			}

			const std::uint32_t batchDrawCount = globalDrawIdx - batchOutputStart;
			if (batchDrawCount == 0)
			{
				i = batchEnd;
				continue;
			}

			m_batchDescMapped[batchIdx] = CullContracts::Batch{.inputStart = batchOutputStart, .drawCount = batchDrawCount, .outputStart = batchOutputStart};

			prepared.batchRenderInfos.push_back(BatchRenderInfo{
			        .pipeline = batchPipeline,
			        .mesh = batchMesh,
			        .meshGeneration = batchMesh ? batchMesh->GetGeneration() : 0,
			        .outputStart = batchOutputStart,
			        .drawCount = batchDrawCount,
			});

			++batchIdx;
			i = batchEnd;
		}

		gpu::ResourceRegistry::FlushMappedBuffer(m_instanceData[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(globalDrawIdx) * sizeof(DrawContracts::InstanceData));
		gpu::ResourceRegistry::FlushMappedBuffer(m_cullInput[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(globalDrawIdx) * sizeof(CullContracts::DrawInput));
		gpu::ResourceRegistry::FlushMappedBuffer(m_batchDesc[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(batchIdx) * sizeof(CullContracts::Batch));
		if (m_skinCopyJobsMapped != nullptr && skinJobCount > 0)
		{
			gpu::ResourceRegistry::FlushMappedBuffer(m_skinCopyJobs[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(skinJobCount) * sizeof(AnimationContracts::SkinCopyJob));
		}
		if (sampleJobsThisFrame > 0)
		{
			gpu::ResourceRegistry::FlushMappedBuffer(m_animationSampleJobs[frameSlot].handle, 0, static_cast<gpu::DeviceSize>(sampleJobsThisFrame) * sizeof(AnimationContracts::AnimatorSampleJob));
		}

		cmdList.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::AllCommands, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

		// The palette this slot already holds is still correct if nothing feeding it moved.
		// An idle or paused character - and every character in the editor, where animation
		// time does not advance - then costs nothing per frame instead of a full sample,
		// blend, node-flatten and palette build.
		const bool poseUnchanged = sampleJobsThisFrame > 0 && animationInputHash != 0ull && m_animationInputHash[frameSlot] == animationInputHash;
		// A pose that should be static but keeps missing means something upstream is
		// perturbing the draw list, which costs a full animation rebuild every frame.
		if (sampleJobsThisFrame > 0 && m_animationInputHash[frameSlot] != animationInputHash && m_animationInputHash[frameSlot] != 0ull)
		{
			AE_VERBOSE(LogCategory::Animation, "Skin palette rebuild on {} slot={}: {} jobs, {} joints, {} node poses", m_debugName, frameSlot, sampleJobsThisFrame, skinJointCursor, nodePoseCursor);
		}
		m_animationInputHash[frameSlot] = animationInputHash;

		if (sampleJobsThisFrame > 0 && !m_debugDisableAnimation && !poseUnchanged)
		{
			AE_VERBOSE(LogCategory::Animation, "Animation dispatch enabled: {} sampleJobs, {} skinJobs", sampleJobsThisFrame, skinJobCount);
			// in parallel (each thread handles one (job, node) pair).
			if (((m_debugAnimPassMask & 1u) != 0u) && m_sharedPipelines != nullptr && m_sharedPipelines->poseInit.IsValid())
			{
				const auto poseInitPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->poseInit);
				AE_PROFILE_ZONE();
				AE_VERBOSE(LogCategory::Animation, "PoseInit: currSampledPosesAddr=0x{:x}, animJobsBDA=0x{:x}", currSampledPosesAddr, m_animationSampleJobs[frameSlot].address);

				cmdList.BindComputePipeline(poseInitPipe.state);
				cmdList.BeginDebugLabel("Animation.PoseInit", 0.9f, 0.6f, 0.3f, 1.0f);
				AE_GPU_ZONE_SCOPED(rawCmd, "Animation.PoseInit");

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

			if (((m_debugAnimPassMask & 2u) != 0u) && m_sharedPipelines != nullptr && m_sharedPipelines->animSample.IsValid() && sampleJobsThisFrame > 0)
			{
				const auto animSamplePipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->animSample);
				cmdList.BindComputePipeline(animSamplePipe.state);
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

				cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead);
			}

			if (m_animationBlendSystem != nullptr && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && ((m_debugAnimPassMask & 2u) != 0u))
			{
				const AnimationContracts::AnimationBlendPush& blendPc = m_animationBlendSystem->GetBlendPush();
				const std::uint32_t blendJobCount = m_animationBlendSystem->GetBlendJobCount();
				const bool blendPushValid = blendPc.blendJobsAddr != 0 && blendPc.sampledPosesAddr != 0;
				if (blendJobCount > 0 && blendPc.jobCount > 0 && blendPushValid)
				{
					AE_PROFILE_ZONE();
					const auto animBlendPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->animBlend);
					cmdList.BindComputePipeline(animBlendPipe.state);
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

		if (((m_debugAnimPassMask & 4u) != 0u) && sampleJobsThisFrame > 0 && !m_debugDisableAnimation && m_sharedPipelines->nodeFlatten.IsValid())
		{
			const auto nodeFlattenPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->nodeFlatten);
			AE_PROFILE_ZONE();
			AE_VERBOSE(LogCategory::Animation, "NodeFlatten: {} batches, {} sampleJobs", animSampleBatchCount, sampleJobsThisFrame);

			cmdList.BindComputePipeline(nodeFlattenPipe.state);
			cmdList.BeginDebugLabel("Animation.NodeFlatten", 0.3f, 0.8f, 0.6f, 1.0f);
			AE_GPU_ZONE_SCOPED(rawCmd, "Animation.NodeFlatten");

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

		if (((m_debugAnimPassMask & 8u) != 0u) && skinJobCount > 0 && !m_debugDisableAnimation)
		{
			AE_PROFILE_ZONE();

			const auto skinPipe = gpu::ResourceRegistry::ResolvePipeline(m_sharedPipelines->skinCopy);
			cmdList.BindComputePipeline(skinPipe.state);
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

		const std::uint32_t totalDraws = globalDrawIdx;
		if (totalDraws == 0)
		{
			return;
		}

		const gpu::DeviceSize inputCmdOffset = 0;
		const gpu::DeviceSize batchDescOffset = 0;

		if (m_outputDrawCapacity > m_maxDraws)
		{
			// write 3 independent output regions.  The 3 frame constant BDAs must have
			const gpu::DeviceSize outputCmdOffset = 0;
			const gpu::DeviceSize cascadeStride = static_cast<gpu::DeviceSize>(m_maxDraws) * sizeof(gpu::DrawIndexedIndirectCommand);

			const CullContracts::MultiPushConstants multiPc{
			        .frameAddrs = {m_multiFrameAddrs[0], m_multiFrameAddrs[1], m_multiFrameAddrs[2]},
			        .instanceDataAddr = prepared.instanceDataAddr,
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
				{
					AE_GPU_ZONE_SCOPED(rawCmd, "CullPass.cullDrawsMulti");
					const std::uint32_t groups = (totalDraws + 63u) / 64u;
					cmdList.Dispatch(groups, 1, 1);
				}
				cmdList.EndDebugLabel();
			}
		}
		else
		{
			const gpu::DeviceSize outputCmdOffset = 0;
			const CullContracts::PushConstants pc{
			        .frameAddr = frameAddr,
			        .instanceDataAddr = prepared.instanceDataAddr,
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

		cmdList.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::AllCommands, gpu::AccessFlags::IndirectCommandRead);

#ifdef TRACY_ENABLE
		{
			AE_PROFILE_PLOT("Animation/SampleJobs", static_cast<int64_t>(sampleJobsThisFrame));
			AE_PROFILE_PLOT("Animation/SkinCopyJobs", static_cast<int64_t>(skinJobCount));
			AE_PROFILE_PLOT("RenderQueue/TotalDraws", static_cast<int64_t>(totalDraws));
		}
#endif
	}

	void RenderQueue::FlushDraw(gpu::CommandList& cmd, std::uint32_t frameIndex, const DrawContracts::LightingAddresses* lighting, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset, const gpu::CullMode* cullModeOverride)
	{
		const PreparedFrame& prepared = m_preparedFrames[frameIndex % kFramesInFlight];
		FlushDrawImpl(cmd, frameIndex, prepared.frameAddr, lighting, overridePipeline, cascadeOffset, cullModeOverride, "RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f);
	}

	void RenderQueue::FlushDrawPush(gpu::CommandList& cmd, std::uint32_t frameIndex, const DrawContracts::LightingAddresses& lighting, const GraphicsPipeline* overridePipeline, std::uint32_t cascadeOffset, const gpu::CullMode* cullModeOverride)
	{
		const PreparedFrame& prepared = m_preparedFrames[frameIndex % kFramesInFlight];
		FlushDrawImpl(cmd, frameIndex, prepared.frameAddr, &lighting, overridePipeline, cascadeOffset, cullModeOverride, "RenderQueue.FlushDraw", 0.85f, 0.60f, 0.18f);
	}

	void RenderQueue::FlushDrawWithFrameAddr(gpu::CommandList& cmd,
	        std::uint32_t frameIndex,
	        const DrawContracts::LightingAddresses* lighting,
	        const gpu::DeviceAddress overrideFrameAddr,
	        const GraphicsPipeline* overridePipeline,
	        std::uint32_t cascadeOffset,
	        const gpu::CullMode* cullModeOverride)
	{
		FlushDrawImpl(cmd, frameIndex, overrideFrameAddr, lighting, overridePipeline, cascadeOffset, cullModeOverride, "RenderQueue.FlushDrawWithAddr", 0.85f, 0.40f, 0.60f);
	}

	void RenderQueue::FlushDrawImpl(gpu::CommandList& cmd,
	        std::uint32_t frameIndex,
	        gpu::DeviceAddress frameAddr,
	        const DrawContracts::LightingAddresses* lighting,
	        const GraphicsPipeline* overridePipeline,
	        std::uint32_t cascadeOffset,
	        const gpu::CullMode* cullModeOverride,
	        const char* debugLabel,
	        float r,
	        float g,
	        float b)
	{
		AE_PROFILE_ZONE();
		if (!cmd.IsValid())
		{
			return;
		}
		const PreparedFrame& prepared = m_preparedFrames[frameIndex % kFramesInFlight];
		if (prepared.batchRenderInfos.empty())
		{
			return;
		}

		cmd.BeginDebugLabel(debugLabel, r, g, b, 1.0f);

		const DrawContracts::PushConstants sharedPc{
		        .frameAddr = frameAddr,
		        .instanceDataAddr = prepared.instanceDataAddr,
		        .skinPaletteAddr = prepared.skinPaletteAddr,
		        .lightDataAddr = lighting ? lighting->lightDataAddr : gpu::DeviceAddress{0},
		        .tileHeadersAddr = lighting ? lighting->tileHeadersAddr : gpu::DeviceAddress{0},
		        .tileLightIndicesAddr = lighting ? lighting->tileLightIndicesAddr : gpu::DeviceAddress{0},
		};
		AE_VERBOSE(LogCategory::Render, "FlushDraw: frameAddr=0x{:x}, instanceDataAddr=0x{:x}, skinPaletteAddr=0x{:x}, batches={}", frameAddr, prepared.instanceDataAddr, prepared.skinPaletteAddr, prepared.batchRenderInfos.size());

		const GraphicsPipeline* lastPipeline = nullptr;
		gpu::BufferHandle lastIndexBuffer{};
		gpu::DeviceSize lastIndexOffset = ~0ull;

		for (const auto& batch: prepared.batchRenderInfos)
		{
			const GraphicsPipeline* activePipeline = overridePipeline != nullptr ? overridePipeline : batch.pipeline;

			if (activePipeline != nullptr && activePipeline != lastPipeline)
			{
				cmd.BindPipeline(activePipeline->GetPipeline());
				if (cullModeOverride != nullptr)
				{
					cmd.SetCullMode(*cullModeOverride);
				}
				lastPipeline = activePipeline;
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
				cmd.DrawIndexedIndirect(gpu::ResourceRegistry::ResolveBufferVkHandle(prepared.indirectHandle),
				        static_cast<gpu::DeviceSize>(prepared.drawBase + cascadeOffset + batch.outputStart) * sizeof(gpu::DrawIndexedIndirectCommand),
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
		m_slotHasAnimatedDraws[idx] = false;
	}

	void RenderQueue::DiscardPending(std::uint32_t slot)
	{
		const auto idx = slot % kFramesInFlight;
		{
			const std::lock_guard lock(m_slotMutexes[idx]);
			m_commandSlots[idx].clear();
			m_slotHasAnimatedDraws[idx] = false;
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
