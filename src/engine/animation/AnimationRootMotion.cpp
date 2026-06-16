#include "animation/AnimationRootMotion.hpp"

#include "gpu/ResourceRegistry.hpp"
#include "gpu/Semaphore.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/LogCategory.hpp"

namespace aether
{
	void AnimationRootMotionSystem::Init(gpu::Device device, std::uint32_t maxEntities)
	{
		m_maxEntities = maxEntities;
		m_prevPositions.resize(static_cast<std::size_t>(maxEntities) * kSlots, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

		const gpu::DeviceSize bufSize = static_cast<gpu::DeviceSize>(maxEntities) * kSlots * sizeof(glm::vec4);
		m_stagingHandle = gpu::ResourceRegistry::CreateMappedBuffer({
		        .size = bufSize,
		        .usage = gpu::BufferUsage::TransferDst,
		        .memoryUsage = gpu::MappedMemoryUsage::GpuToCpu,
		        .debugName = "AnimationRootMotion.Staging",
		});
		AE_ASSERT_ALWAYS(m_stagingHandle.IsValid(), "AnimationRootMotionSystem: failed to allocate staging buffer.");

		const gpu::MappedBufferView view = gpu::ResourceRegistry::ResolveMappedBuffer(m_stagingHandle);
		m_stagingMappedData = view.mappedPtr;

		m_timelineSemaphore = gpu::CreateTimelineSemaphore({
		        .device = device,
		        .initialValue = 0,
		        .debugName = "AnimationRootMotion.Timeline",
		});
		AE_ASSERT_ALWAYS(m_timelineSemaphore != nullptr, "AnimationRootMotionSystem: failed to create timeline semaphore.");

		m_currentTimelineValue = 0;
	}

	void AnimationRootMotionSystem::Shutdown(gpu::Device device)
	{
		if (m_timelineSemaphore != nullptr)
		{
			gpu::DestroyTimelineSemaphore(device, m_timelineSemaphore);
			m_timelineSemaphore = nullptr;
		}
		if (m_stagingHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_stagingHandle);
			m_stagingHandle = {};
			m_stagingMappedData = nullptr;
		}
		m_prevPositions.clear();
	}

	void AnimationRootMotionSystem::BeginFrame(gpu::Device device, std::uint32_t frameIndex)
	{
		if (frameIndex == 0)
		{
			return;
		}

		if (!gpu::WaitTimelineSemaphore(device, m_timelineSemaphore, frameIndex))
		{
			AE_WARN(LogCategory::Animation, "AnimationRootMotionSystem::BeginFrame: gpu::WaitTimelineSemaphore failed (expected success or timeout).");
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
		for (const auto& [entity, rootMotion, physState]: view.each())
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
