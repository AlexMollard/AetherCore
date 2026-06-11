#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace aether
{
	class World;

	class AnimationRootMotionSystem
	{
	public:
		void Init(void* allocator, void* device, std::uint32_t maxEntities);
		void Shutdown(void* device);

		void BeginFrame(void* device, std::uint32_t frameIndex);

		void* GetTimelineSemaphore() const
		{
			return m_timelineSemaphore;
		}

		void ApplyDelta(World& world, float dt, std::uint32_t frameIndex);

	private:
		struct UniqueBuffer
		{
			void* m_buffer = nullptr;
			void* m_allocation = nullptr;
			void* mMappedData = nullptr;
			void* m_allocator = nullptr;
		};

		UniqueBuffer m_stagingBuffer;
		std::vector<glm::vec4> m_prevPositions;
		void* m_timelineSemaphore = nullptr;
		std::uint32_t m_maxEntities = 0;
		std::uint64_t m_currentTimelineValue = 0;

		static constexpr std::uint32_t kSlots = 2u;
	};
} // namespace aether
