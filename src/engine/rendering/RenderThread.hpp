#pragma once

#include <atomic>
#include <thread>

#include "utils/coro/Channel.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	class AetherCore;

	// Dedicated render thread that owns all Vulkan submission work.
	//
	// Frame pipeline (fully pipelined):
	//   Game thread   : Sim N -> SubmitFrame(N) -> Sim N+1 -> SubmitFrame(N+1) -> ...
	//   Render thread :              Read N -> Exec N            Read N+1 -> Exec N+1 -> ...
	//
	// Synchronisation uses a bounded channel (capacity = 2 = double-buffered).
	// SubmitFrame writes to the channel and returns immediately -- the game thread
	// continues with frame N+1's simulation while the render thread executes frame N.
	//
	// All shared render data is deep-copied per frame slot, so there are no
	// data races between the game and render threads.
	class RenderThread
	{
	public:
		RenderThread();

		void Start(AetherCore& engine);
		void Stop();

		// Hand off a completed render packet to the render thread.
		// Returns immediately (microsecond latency).  The render thread picks
		// up the packet from the channel and processes it asynchronously.
		void SubmitFrame(RenderFramePacket packet);

		// Block until the render thread has no pending work.
		// Call before tearing down Vulkan resources.
		void WaitIdle();

		// Pause/resume render thread during critical sections (e.g. script hot-reload).
		// When SetReloadInProgress(true) is called, the render thread stops executing
		// frames after the current one completes. Call SetReloadInProgress(false) to resume.
		static void SetReloadInProgress(bool inProgress);
		static bool IsReloadInProgress();

	private:
		void ThreadLoop();

		AetherCore* m_engine = nullptr;
		std::thread m_thread;
		coro::channel<RenderFramePacket> m_channel;

		// Tracks the index of the last fully-executed frame (for shutdown /
		// debugging / statistics).  Not used for per-frame synchronisation.
		std::atomic<std::uint64_t> m_lastCompletedFrameIndex{0};
		std::atomic<bool> m_shutdown{false};
		static std::atomic<bool> s_reloadInProgress;

		// True when render thread is not executing a frame - used to synchronize
		// with DoReload so we don't call WaitIdle() while a frame is in flight.
		std::atomic<bool> m_isIdle{true};
	};

} // namespace aether
