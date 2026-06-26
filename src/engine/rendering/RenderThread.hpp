#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
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
	// Synchronisation uses a bounded channel (capacity = kMaxFramesInFlight).
	// SubmitFrame writes to the channel and returns immediately -- the game thread
	// continues with frame N+1's simulation while the render thread executes frame N.
	//
	// All shared render data is deep-copied per frame slot, so there are no
	// data races between the game and render threads.
	//
	// Thread safety: Start/Stop/WaitIdle are main-thread-only after init.
	// SetReloadInProgress is main-thread-only. SubmitFrame is game-thread-only.
	class RenderThread
	{
	public:
		RenderThread();

		RenderThread(const RenderThread&) = delete;
		RenderThread& operator=(const RenderThread&) = delete;
		RenderThread(RenderThread&&) = delete;
		RenderThread& operator=(RenderThread&&) = delete;

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
		void SetReloadInProgress(bool inProgress);
		void WaitPaused();
		[[nodiscard]] bool IsReloadInProgress() const;

	private:
		void ThreadLoop();

		// Apply platform-specific thread configuration (priority, name, scheduling).
		// Called inside ThreadLoop() on the newly-started thread.
		static void ConfigureThisThread();

		AetherCore* m_engine = nullptr;
		std::thread m_thread;
		coro::channel<RenderFramePacket> m_channel;

		std::mutex m_reloadMutex;
		std::condition_variable m_reloadCv;

		// Tracks the index of the last fully-executed frame (for shutdown /
		// debugging / statistics).  Not used for per-frame synchronisation.
		std::atomic<std::uint64_t> m_lastCompletedFrameIndex{0};
		std::atomic<bool> m_shutdown{false};
		std::atomic<bool> m_reloadInProgress{false};

		// True when render thread is not executing a frame - used to synchronize
		// with DoReload so we don't call WaitIdle() while a frame is in flight.
		std::atomic<bool> m_isIdle{true};
	};

} // namespace aether
