#pragma once

#include <mutex>

namespace aether::vulkan
{
	// Vulkan requires vkQueueSubmit / vkQueueSubmit2 / vkQueuePresentKHR on a given
	// queue to be *externally synchronized* - the driver does no locking of its own.
	//
	// Uploads live on the TransferManager's own queue and textures finish with host
	// image copy (no queue at all), so in steady state the graphics queue is only
	// submitted from the render loop. This lock remains the correctness backstop for
	// the paths that still share it - screenshots, the imgui viewport renderer, the
	// rare one-shot fallback - and for hardware where the transfer queue falls back to
	// aliasing the graphics queue.
	//
	// One lock covers every queue rather than one per queue: submission only *enqueues*
	// work (the GPU still runs graphics, compute and DMA concurrently), so the hold is
	// a few microseconds and splitting it buys nothing measurable. It must also stay a
	// single lock while the graphics and present queues can be the same VkQueue.
	//
	// Held only around the submit/present call itself - never across a fence wait.
	inline std::mutex& QueueSubmitMutex()
	{
		static std::mutex mutex;
		return mutex;
	}
} // namespace aether::vulkan
