#pragma once

#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>
#include "gpu/GpuTypes.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>

namespace aether
{
	class World;

	class AnimationRootMotionSystem
	{
	public:
		void Init(VmaAllocator allocator, VkDevice device, std::uint32_t maxEntities);
		void Shutdown(VkDevice device);

		void BeginFrame(VkDevice device, std::uint32_t frameIndex);

		void RecordCopyHipsPosition(VkCommandBuffer cmd, std::uint32_t hipNodeIdx, gpu::DeviceAddress nodeGlobalTransformsAddr, std::uint32_t frameIndex);

		VkTimelineSemaphoreSubmitInfo GetSignalSemaphoreSubmitInfo(std::uint32_t frameIndex) const;

		VkSemaphore GetTimelineSemaphore() const
		{
			return m_timelineSemaphore;
		}

		void ApplyDelta(World& world, float dt, std::uint32_t frameIndex);

	private:
		struct UniqueBuffer
		{
			VkBuffer m_buffer = VK_NULL_HANDLE;
			VmaAllocation m_allocation = VK_NULL_HANDLE;
			void* mMappedData = nullptr;
			VmaAllocator m_allocator = VK_NULL_HANDLE;
		};

		UniqueBuffer m_stagingBuffer;
		std::vector<glm::vec4> m_prevPositions;
		VkSemaphore m_timelineSemaphore = VK_NULL_HANDLE;
		std::uint32_t m_maxEntities = 0;
		std::uint64_t m_currentTimelineValue = 0;

		static constexpr std::uint32_t kSlots = 2u;
	};
} // namespace aether
