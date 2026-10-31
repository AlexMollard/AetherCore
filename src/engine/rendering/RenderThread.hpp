#pragma once

#include <thread>

#include "utils/coro/Channel.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderThreadState.hpp"

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

		RenderThreadState m_state;
	};

} // namespace aether
