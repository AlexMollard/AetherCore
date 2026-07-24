#pragma once

#include <mutex>

namespace aether::vulkan
{
	// Vulkan requires vkQueueSubmit / vkQueueSubmit2 / vkQueuePresentKHR on a given
	// queue to be *externally synchronized* - the driver does no locking of its own.
	//
	// AetherCore submits from more than one thread: the render loop drives the
	// swapchain, while asset uploads (OneShotCmd, GpuHeap), the screenshot service and
	// the imgui viewport renderer all submit from whichever thread asked for the work.
	// Without a shared lock those race, which the validation layer reports as
	// "THREADING ERROR : object of type VkQueue is simultaneously used in ...".
	//
	// One lock covers every queue rather than one per queue: submission only *enqueues*
	// work (the GPU still runs graphics and async compute concurrently), so the hold is
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
