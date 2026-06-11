#include "animation/AnimationRootMotion.hpp"
#include "gpu/GpuTypes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include <cstring>
#include <stdexcept>

// Transitional: cast opaque void* back to Vulkan types during P5 migration.
#define AE_VK_DEVICE(ptr)       (static_cast<VkDevice>(ptr))
#define AE_VK_ALLOCATOR(ptr)    (static_cast<VmaAllocator>(ptr))
#define AE_VK_SEMAPHORE(ptr)    (static_cast<VkSemaphore>(ptr))

namespace aether
{
	void AnimationRootMotionSystem::Init(void* allocator, void* device, std::uint32_t maxEntities)
	{
		m_maxEntities = maxEntities;
		m_prevPositions.resize(static_cast<std::size_t>(maxEntities) * kSlots, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

		VmaAllocator vkAllocator = AE_VK_ALLOCATOR(allocator);
		VkDevice vkDevice = AE_VK_DEVICE(device);

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
		VmaAllocation vkAlloc = VK_NULL_HANDLE;
		if (vmaCreateBuffer(vkAllocator, &stagingInfo, &allocInfo, &buf, &vkAlloc, nullptr) != VK_SUCCESS)
		{
			AE_ASSERT_ALWAYS(false, "AnimationRootMotionSystem: failed to allocate staging buffer.");
		}
		m_stagingBuffer.m_buffer = buf;
		m_stagingBuffer.m_allocation = vkAlloc;
		m_stagingBuffer.m_allocator = allocator;

		void* mapped = nullptr;
		vmaMapMemory(vkAllocator, vkAlloc, &mapped);
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
		VkSemaphore sem = VK_NULL_HANDLE;
		if (vkCreateSemaphore(vkDevice, &semInfo, nullptr, &sem) != VK_SUCCESS)
		{
			AE_ASSERT_ALWAYS(false, "AnimationRootMotionSystem: failed to create timeline semaphore.");
		}
		m_timelineSemaphore = sem;

		m_currentTimelineValue = 0;
	}

	void AnimationRootMotionSystem::Shutdown(void* device)
	{
		VkDevice vkDevice = AE_VK_DEVICE(device);

		if (m_timelineSemaphore != nullptr)
		{
			vkDestroySemaphore(vkDevice, AE_VK_SEMAPHORE(m_timelineSemaphore), nullptr);
			m_timelineSemaphore = nullptr;
		}
		if (m_stagingBuffer.m_buffer != nullptr)
		{
			vmaDestroyBuffer(AE_VK_ALLOCATOR(m_stagingBuffer.m_allocator), static_cast<VkBuffer>(m_stagingBuffer.m_buffer), static_cast<VmaAllocation>(m_stagingBuffer.m_allocation));
			m_stagingBuffer.m_buffer = nullptr;
			m_stagingBuffer.m_allocation = nullptr;
			m_stagingBuffer.mMappedData = nullptr;
		}
		m_prevPositions.clear();
	}

	void AnimationRootMotionSystem::BeginFrame(void* device, std::uint32_t frameIndex)
	{
		if (frameIndex == 0)
		{
			return;
		}

		VkDevice vkDevice = AE_VK_DEVICE(device);
		VkSemaphore sem = AE_VK_SEMAPHORE(m_timelineSemaphore);

		const std::uint64_t waitValue = frameIndex;
		VkSemaphoreWaitInfo waitInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		        .flags = 0,
		        .semaphoreCount = 1,
		        .pSemaphores = &sem,
		        .pValues = &waitValue,
		};

		VkResult res = vkWaitSemaphores(vkDevice, &waitInfo, UINT64_MAX);
		if (res != VK_SUCCESS && res != VK_TIMEOUT)
		{
			AE_WARN(LogCategory::Animation, "AnimationRootMotionSystem::BeginFrame: vkWaitSemaphores returned {} (expected success or timeout)", static_cast<int>(res));
		}
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
