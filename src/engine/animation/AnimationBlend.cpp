#include "animation/AnimationBlend.hpp"
#include "animation/AnimationDatabase.hpp"
#include "gpu/GpuTypes.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include <entt/entt.hpp>

#include "utils/Profiler.hpp"

namespace aether
{
	void AnimationBlendSystem::Init(gpu::DeviceSize maxBlendJobCount, std::uint32_t nodeCount)
	{
		AE_PROFILE_ZONE();
		m_nodeCount = nodeCount;
		m_blendJobs.resize(static_cast<std::size_t>(maxBlendJobCount));

		const gpu::MappedBufferDesc desc{
		        .size = maxBlendJobCount * sizeof(AnimationContracts::AnimatorBlendJob),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "AnimationBlend.Jobs",
		};
		m_handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_handle.IsValid())
		{
			Throw(AetherError::Engine("AnimationBlendSystem: CreateMappedBuffer failed"));
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_handle);
		m_mappedBlendJobs = static_cast<AnimationContracts::AnimatorBlendJob*>(view.mappedPtr);
		m_address = view.deviceAddress;
	}

	void AnimationBlendSystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_handle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_handle);
		}
		m_handle = {};
		m_address = 0;
		m_blendJobs.clear();
		m_mappedBlendJobs = nullptr;
	}

	void AnimationBlendSystem::PopulateBlendJobs(World& world, const AnimationDatabase& animDb)
	{
		AE_PROFILE_ZONE();
		m_writtenJobCount = 0;

		auto& reg = world.GetRegistry();

		auto skinnedView = reg.view<SkinnedMeshComponent>();
		std::uint32_t jobIdx = 0;

		for (const auto& [entity, skinned]: skinnedView.each())
		{
			if (jobIdx >= static_cast<std::uint32_t>(m_blendJobs.size()))
			{
				AE_WARN(LogCategory::Animation, "AnimationBlendSystem: blend job buffer overflow at {} jobs.", jobIdx);
				break;
			}

			const AnimationBlendComponent* blendComp = reg.try_get<AnimationBlendComponent>(entity);
			if (blendComp == nullptr || !blendComp->inTransition)
			{
				++jobIdx;
				continue;
			}

			AnimationContracts::AnimatorBlendJob job{};
			job.primaryClipIndex = blendComp->primaryClip;
			job.primaryTime = skinned.animTime;
			job.secondaryClipIndex = blendComp->secondaryClip;
			job.secondaryTime = skinned.animTime;
			job.blendWeight = blendComp->blendWeight;
			job.nodePoseOffset = skinned.nodePoseOffset;
			job.nodeCount = m_nodeCount;
			job.clipsAddr = animDb.GetClipsAddr();
			job.channelsAddr = animDb.GetChannelsAddr();
			job.timesAddr = animDb.GetTimesAddr();
			job.valuesAddr = animDb.GetValuesAddr();
			job.clipCount = animDb.GetClipCount();

			m_mappedBlendJobs[jobIdx] = job;
			m_blendJobs[jobIdx] = job;
			++jobIdx;
			++m_writtenJobCount;
		}

		gpu::ResourceRegistry::FlushMappedBuffer(m_handle, 0, static_cast<gpu::DeviceSize>(-1));
	}

	void AnimationBlendSystem::BuildBlendPush(const AnimationDatabase& animDb, gpu::DeviceAddress sampledPosesAddr)
	{
		m_blendPush = AnimationContracts::AnimationBlendPush{
		        .animDbClipsAddr = animDb.GetClipsAddr(),
		        .animDbChannelsAddr = animDb.GetChannelsAddr(),
		        .animDbTimesAddr = animDb.GetTimesAddr(),
		        .animDbValuesAddr = animDb.GetValuesAddr(),
		        .bindTranslationsAddr = animDb.GetBindTranslationsAddr(),
		        .bindRotationsAddr = animDb.GetBindRotationsAddr(),
		        .bindScalesAddr = animDb.GetBindScalesAddr(),
		        .blendJobsAddr = m_address,
		        .sampledPosesAddr = sampledPosesAddr,
		        .jobCount = m_writtenJobCount,
		};
	}
} // namespace aether
