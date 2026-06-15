#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

#include "gpu/GpuHandles.hpp"
#include "gpu/Semaphore.hpp"

namespace aether
{
	class World;

	class AnimationRootMotionSystem
	{
	public:
		void Init(gpu::Device device, std::uint32_t maxEntities);
		void Shutdown(gpu::Device device);

		void BeginFrame(gpu::Device device, std::uint32_t frameIndex);

		[[nodiscard]] gpu::TimelineSemaphoreHandle GetTimelineSemaphore() const noexcept
		{
			return m_timelineSemaphore;
		}

		void ApplyDelta(World& world, float dt, std::uint32_t frameIndex);

	private:
		gpu::BufferHandle m_stagingHandle{};
		void* m_stagingMappedData = nullptr;
		std::vector<glm::vec4> m_prevPositions;
		gpu::TimelineSemaphoreHandle m_timelineSemaphore = nullptr;
		std::uint32_t m_maxEntities = 0;
		std::uint64_t m_currentTimelineValue = 0;

		static constexpr std::uint32_t kSlots = 2u;
	};
} // namespace aether
