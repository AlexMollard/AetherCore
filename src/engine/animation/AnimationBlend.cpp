#include "animation/AnimationBlend.hpp"
#include "animation/AnimationDatabase.hpp"
#include "gpu/GpuTypes.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include <entt/entt.hpp>

namespace aether
{
	void AnimationBlendSystem::Init(VmaAllocator allocator, VkDevice device, VkDeviceSize maxBlendJobCount, std::uint32_t nodeCount)
	{
		m_nodeCount = nodeCount;
		m_blendJobs.resize(static_cast<std::size_t>(maxBlendJobCount));

		AE_EXPECT_OR_THROW(buffer,
		        UniqueBuffer::CreateMapped(
		                allocator, device, static_cast<VkDeviceSize>(maxBlendJobCount) * sizeof(AnimationContracts::AnimatorBlendJob), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "AnimationBlend.Jobs"));
		m_blendJobsBuffer = std::move(buffer);

		VmaAllocationInfo info = m_blendJobsBuffer.GetAllocationInfo();
		m_mappedBlendJobs = static_cast<AnimationContracts::AnimatorBlendJob*>(info.pMappedData);
	}

	void AnimationBlendSystem::Shutdown(VkDevice /*device*/)
	{
		m_blendJobsBuffer.Reset();
		m_blendJobs.clear();
		m_mappedBlendJobs = nullptr;
	}

	void AnimationBlendSystem::UpdateBlendWeights(World& world, float dt)
	{
		auto& reg = world.GetRegistry();
		auto view = reg.view<AnimationBlendComponent>();
		for (const auto& [entity, blendComp]: view.each())
		{
			if (!blendComp.inTransition)
			{
				continue;
			}

			blendComp.blendWeight -= blendComp.transitionSpeed * dt;
			if (blendComp.blendWeight <= 0.0f)
			{
				blendComp.blendWeight = 1.0f;
				blendComp.primaryClip = blendComp.secondaryClip;
				blendComp.secondaryClip = 0;
				blendComp.inTransition = false;
			}
		}
	}

	void AnimationBlendSystem::PopulateBlendJobs(World& world, const AnimationDatabase& animDb, std::uint32_t /*frameIndex*/)
	{
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

			AnimationBlendComponent* blendComp = reg.try_get<AnimationBlendComponent>(entity);
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

		AE_EXPECT_OR_THROW_VOID(m_blendJobsBuffer.FlushMapped());
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
		        .blendJobsAddr = m_blendJobsBuffer.GetDeviceAddress(),
		        .sampledPosesAddr = sampledPosesAddr,
		        .jobCount = m_writtenJobCount,
		};
	}
} // namespace aether
