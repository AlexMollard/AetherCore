#pragma once

#include "rendering/GpuContracts.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/World.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include <vector>

namespace aether
{
	class AnimationBlendSystem
	{
	public:
		void Init(VmaAllocator allocator, VkDevice device, VkDeviceSize maxBlendJobCount, std::uint32_t nodeCount);
		void Shutdown(VkDevice device);

		void UpdateBlendWeights(World& world, float dt);

		void PopulateBlendJobs(World& world, const AnimationDatabase& animDb, std::uint32_t frameIndex);

		void BuildBlendPush(const AnimationDatabase& animDb, VkDeviceAddress sampledPosesAddr);

		const AnimationContracts::AnimationBlendPush& GetBlendPush() const
		{
			return m_blendPush;
		}

		std::uint32_t GetBlendJobCount() const
		{
			return m_writtenJobCount;
		}

		VkDeviceAddress GetBlendJobsDeviceAddress() const
		{
			return m_blendJobsBuffer.GetDeviceAddress();
		}

	private:
		UniqueBuffer m_blendJobsBuffer;
		std::vector<AnimationContracts::AnimatorBlendJob> m_blendJobs;
		AnimationContracts::AnimationBlendPush m_blendPush{};
		std::uint32_t m_nodeCount = 0;
		std::uint32_t m_writtenJobCount = 0;
		AnimationContracts::AnimatorBlendJob* m_mappedBlendJobs = nullptr;
	};
} // namespace aether
