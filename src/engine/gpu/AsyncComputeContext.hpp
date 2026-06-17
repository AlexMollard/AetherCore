#pragma once

#include <array>
#include <cstdint>

#include "gpu/CommandList.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class GpuDevice;

	class AsyncComputeContext
	{
	public:
		AsyncComputeContext() = default;
		~AsyncComputeContext();

		AsyncComputeContext(const AsyncComputeContext&) = delete;
		AsyncComputeContext& operator=(const AsyncComputeContext&) = delete;

		void Init(GpuDevice& gpu);
		void Shutdown(GpuDevice& gpu);

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		[[nodiscard]] std::uint64_t GetTimelineSemaphoreHandle() const
		{
			return m_timelineSemaphoreHandle;
		}

		[[nodiscard]] std::uint64_t GetCurrentTimelineValue() const
		{
			return m_timelineValue;
		}

	private:
		struct FrameResources
		{
			std::uint64_t commandPool = 0;
			std::uint64_t commandBuffer = 0;
			std::uint64_t fence = 0;
		};

		std::array<FrameResources, kMaxFramesInFlight> m_frames{};
		std::uint64_t m_timelineSemaphoreHandle = 0;
		std::uint64_t m_timelineValue = 0;
		GpuDevice* m_gpu = nullptr;
		bool m_enabled = false;
		bool m_initialized = false;
	};
} // namespace aether
