#include "animation/AnimationRootMotion.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include <cstring>
#include <stdexcept>

namespace aether
{
	void AnimationRootMotionSystem::Init(VmaAllocator allocator, VkDevice device, std::uint32_t maxEntities)
	{
		m_maxEntities = maxEntities;
		m_prevPositions.resize(static_cast<std::size_t>(maxEntities) * kSlots, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

		const VkBufferCreateInfo stagingInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = static_cast<VkDeviceSize>(maxEntities) * kSlots * sizeof(glm::vec4),
		        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo allocInfo{
		        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		        .usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
		};
		VkBuffer buf = VK_NULL_HANDLE;
		VmaAllocation alloc = VK_NULL_HANDLE;
		if (vmaCreateBuffer(allocator, &stagingInfo, &allocInfo, &buf, &alloc, nullptr) != VK_SUCCESS)
		{
			AE_ASSERT_ALWAYS(false, "AnimationRootMotionSystem: failed to allocate staging buffer.");
		}
		m_stagingBuffer.m_buffer = buf;
		m_stagingBuffer.m_allocation = alloc;
		m_stagingBuffer.m_allocator = allocator;

		void* mapped = nullptr;
		vmaMapMemory(allocator, alloc, &mapped);
		m_stagingBuffer.mMappedData = mapped;

		VkSemaphoreTypeCreateInfo timelineTypeInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		        .initialValue = 0,
		};
		VkSemaphoreCreateInfo semInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		        .pNext = &timelineTypeInfo,
		};
		if (vkCreateSemaphore(device, &semInfo, nullptr, &m_timelineSemaphore) != VK_SUCCESS)
		{
			AE_ASSERT_ALWAYS(false, "AnimationRootMotionSystem: failed to create timeline semaphore.");
		}

		m_currentTimelineValue = 0;
	}

	void AnimationRootMotionSystem::Shutdown(VkDevice device)
	{
		if (m_timelineSemaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(device, m_timelineSemaphore, nullptr);
			m_timelineSemaphore = VK_NULL_HANDLE;
		}
		if (m_stagingBuffer.m_buffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_stagingBuffer.m_allocator, m_stagingBuffer.m_buffer, m_stagingBuffer.m_allocation);
			m_stagingBuffer.m_buffer = VK_NULL_HANDLE;
			m_stagingBuffer.m_allocation = VK_NULL_HANDLE;
			m_stagingBuffer.mMappedData = nullptr;
		}
		m_prevPositions.clear();
	}

	void AnimationRootMotionSystem::BeginFrame(VkDevice device, std::uint32_t frameIndex)
	{
		if (frameIndex == 0)
		{
			return;
		}

		const std::uint64_t waitValue = frameIndex;
		VkSemaphoreWaitInfo waitInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		        .flags = 0,
		        .semaphoreCount = 1,
		        .pSemaphores = &m_timelineSemaphore,
		        .pValues = &waitValue,
		};

		VkResult res = vkWaitSemaphores(device, &waitInfo, UINT64_MAX);
		if (res != VK_SUCCESS && res != VK_TIMEOUT)
		{
			AE_WARN(LogCategory::Animation, "AnimationRootMotionSystem::BeginFrame: vkWaitSemaphores returned {} (expected success or timeout)", static_cast<int>(res));
		}
	}

	void AnimationRootMotionSystem::RecordCopyHipsPosition(VkCommandBuffer cmd, std::uint32_t hipNodeIdx, VkDeviceAddress nodeGlobalTransformsAddr, std::uint32_t frameIndex)
	{
		const std::uint32_t slot = frameIndex % kSlots;
		const VkDeviceSize dstOffset = static_cast<VkDeviceSize>(slot * m_maxEntities) * sizeof(glm::vec4);
		const VkDeviceSize srcOffset = nodeGlobalTransformsAddr + static_cast<VkDeviceSize>(hipNodeIdx) * 64;

		VkBufferCopy copyRegion{
		        .srcOffset = srcOffset,
		        .dstOffset = dstOffset,
		        .size = static_cast<VkDeviceSize>(m_maxEntities) * sizeof(glm::vec4),
		};

		vkCmdCopyBuffer(cmd, VK_NULL_HANDLE, m_stagingBuffer.m_buffer, 1, &copyRegion);

		const VkMemoryBarrier2 transferBarrier{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
		        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		};
		VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .memoryBarrierCount = 1,
		        .pMemoryBarriers = &transferBarrier,
		};
		vkCmdPipelineBarrier2(cmd, &dep);

		m_currentTimelineValue = frameIndex + 1;
	}

	VkTimelineSemaphoreSubmitInfo AnimationRootMotionSystem::GetSignalSemaphoreSubmitInfo(std::uint32_t /*frameIndex*/) const
	{
		return VkTimelineSemaphoreSubmitInfo{
		        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		        .waitSemaphoreValueCount = 0,
		        .pWaitSemaphoreValues = nullptr,
		        .signalSemaphoreValueCount = 1,
		        .pSignalSemaphoreValues = &m_currentTimelineValue,
		};
	}

	void AnimationRootMotionSystem::ApplyDelta(World& world, float /*dt*/, std::uint32_t frameIndex)
	{
		if (frameIndex == 0)
		{
			return;
		}

		const std::uint32_t readSlot = (frameIndex - 1) % kSlots;
		const std::uint32_t entityCount = m_maxEntities;

		auto& reg = world.GetRegistry();
		auto view = reg.view<RootMotionComponent, PhysicsStateComponent>();

		std::uint32_t entityIdx = 0;
		for (auto [entity, rootMotion, physState]: view.each())
		{
			if (!rootMotion.enabled)
			{
				++entityIdx;
				continue;
			}
			if (entityIdx >= entityCount)
			{
				break;
			}

			const glm::vec4 prevPos = m_prevPositions[readSlot * entityCount + entityIdx];
			const glm::vec3 currHipsPos = glm::vec3(prevPos);

			if (rootMotion.prevHipsWorldPos.x != 0.0f || rootMotion.prevHipsWorldPos.y != 0.0f || rootMotion.prevHipsWorldPos.z != 0.0f)
			{
				const glm::vec3 delta = currHipsPos - rootMotion.prevHipsWorldPos;

				if (rootMotion.applyToPhysics)
				{
					physState.currPosition += delta;
				}

				rootMotion.accumulatedDelta += delta;
			}

			rootMotion.prevHipsWorldPos = currHipsPos;
			++entityIdx;
		}
	}
} // namespace aether
