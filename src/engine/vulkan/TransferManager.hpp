#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "gpu/CommandList.hpp"
#include "vulkan/volk.hpp"

namespace aether::vulkan
{
	// Owns the engine's upload path: a transfer queue (a dedicated DMA family when the
	// hardware exposes one, falling back to the graphics family otherwise), its command
	// pool, and one timeline semaphore that orders every upload.
	//
	// Each Submit records a command buffer on the calling thread, submits it to the
	// transfer queue signalling the next timeline value, and returns that value as a
	// Ticket. Callers never block: the frame's graphics submission waits on the latest
	// ticket (see GpuDevice::SubmitAndPresent), which both orders the copies before any
	// rendering that consumes them and makes the transferred memory visible. Buffers
	// shared across families are created VK_SHARING_MODE_CONCURRENT (see
	// ResourceRegistry::SetSharedBufferQueueFamilies), so no queue-family ownership
	// transfer barriers are required.
	//
	// Thread-safe: recording is serialised on an internal mutex, and the submit itself
	// additionally takes the global QueueSubmitMutex so the graphics-family fallback
	// (where the transfer queue aliases the graphics queue) stays externally
	// synchronised against the render loop.
	class TransferManager
	{
	public:
		// Timeline value the upload signals when complete. 0 = "no upload" and is
		// always complete.
		using Ticket = std::uint64_t;

		TransferManager() = default;
		~TransferManager();

		TransferManager(const TransferManager&) = delete;
		TransferManager& operator=(const TransferManager&) = delete;
		TransferManager(TransferManager&&) = delete;
		TransferManager& operator=(TransferManager&&) = delete;

		void Initialize(VkDevice device, VkQueue transferQueue, std::uint32_t transferFamily, std::uint32_t graphicsFamily);
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const
		{
			return m_device != VK_NULL_HANDLE;
		}

		// Record and submit one upload batch. `record` runs synchronously on the calling
		// thread; `onComplete` (optional) runs once the GPU has finished the batch - used
		// to reclaim staging memory - on whichever thread next touches the manager.
		[[nodiscard]] Ticket Submit(const std::function<void(gpu::CommandList&)>& record, std::function<void()> onComplete = {});

		[[nodiscard]] bool IsComplete(Ticket ticket) const;
		void WaitFor(Ticket ticket);

		// Latest submitted ticket - what a graphics submission should wait on.
		[[nodiscard]] Ticket LastSubmitted() const
		{
			return m_lastSubmitted.load(std::memory_order_acquire);
		}

		[[nodiscard]] VkSemaphore TimelineSemaphore() const
		{
			return m_timeline;
		}

		[[nodiscard]] std::uint32_t QueueFamily() const
		{
			return m_transferFamily;
		}

		// True when the uploads run on a genuinely separate queue family (async DMA).
		[[nodiscard]] bool HasDedicatedFamily() const
		{
			return m_transferFamily != m_graphicsFamily;
		}

	private:
		struct InFlight
		{
			Ticket value = 0;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			std::function<void()> onComplete;
		};

		// Reclaim command buffers / run completions for finished tickets. Caller holds m_mutex.
		void CollectCompleted();

		VkDevice m_device = VK_NULL_HANDLE;
		VkQueue m_queue = VK_NULL_HANDLE;
		std::uint32_t m_transferFamily = 0;
		std::uint32_t m_graphicsFamily = 0;
		VkSemaphore m_timeline = VK_NULL_HANDLE;
		VkCommandPool m_pool = VK_NULL_HANDLE;
		Ticket m_nextValue = 1;
		std::atomic<Ticket> m_lastSubmitted{0};
		std::vector<InFlight> m_inFlight;
		std::mutex m_mutex;
	};
} // namespace aether::vulkan
