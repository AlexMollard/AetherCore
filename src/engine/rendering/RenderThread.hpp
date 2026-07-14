#pragma once

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

#include "utils/coro/Channel.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	class AetherCore;

	// Dedicated render thread that owns all Vulkan submission work.
	class RenderThread
	{
	public:
		RenderThread();
		~RenderThread() = default;

		RenderThread(const RenderThread&) = delete;
		RenderThread& operator=(const RenderThread&) = delete;
		RenderThread(RenderThread&&) = delete;
		RenderThread& operator=(RenderThread&&) = delete;

		void Start(AetherCore& engine);
		void Stop();

		// Hand off a completed render packet to the render thread.
		void SubmitFrame(RenderFramePacket packet);
		void WaitUntilFrameCompleted(std::uint64_t frameIndex);

		// Block until the render thread has no pending work.
		void WaitIdle();

		// Pause/resume render thread during critical sections (e.g. script hot-reload).
		void SetReloadInProgress(bool inProgress);
		void WaitPaused();
		[[nodiscard]] bool IsReloadInProgress() const;

	private:
		void ThreadLoop();

		// Apply platform-specific thread configuration (priority, name, scheduling).
		static void ConfigureThisThread();

		AetherCore* m_engine = nullptr;
		std::thread m_thread;
		coro::channel<RenderFramePacket> m_channel;

		std::mutex m_reloadMutex;
		std::condition_variable m_reloadCv;
		std::mutex m_completionMutex;
		std::condition_variable m_completionCv;

		// Tracks the last fully executed frame. The game thread uses this as
		std::atomic<std::uint64_t> m_lastCompletedFrameIndex{std::numeric_limits<std::uint64_t>::max()};
		std::atomic<bool> m_shutdown{false};
		std::atomic<bool> m_reloadInProgress{false};

		// True when render thread is not executing a frame - used to synchronize
		std::atomic<bool> m_isIdle{true};
	};

} // namespace aether
