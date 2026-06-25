#include "rendering/RenderThread.hpp"

#include "AetherCore.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/Swapchain.hpp"

// ---------------------------------------------------------------------------
// Platform thread configuration
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#	include <windows.h>
#elif defined(__linux__)
#	include <pthread.h>
#endif

namespace aether
{
	RenderThread::RenderThread()
	      : m_channel(Swapchain::kMaxFramesInFlight) // match swapchain depth: game thread can write
	                                                 // up to 3 frames ahead before backpressure kicks in
	{
	}

	void RenderThread::Start(AetherCore& engine)
	{
		m_engine = &engine;
		m_thread = std::thread([this] { ThreadLoop(); });
	}

	void RenderThread::Stop()
	{
		{
			std::lock_guard lock(m_reloadMutex);
			m_shutdown = true;
		}
		m_reloadCv.notify_one();
		m_channel.close();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SubmitFrame(RenderFramePacket packet)
	{
		AE_PROFILE_ZONE();

		// Write to the channel and return immediately.
		// The render thread picks up the packet asynchronously.
		// If all channel slots are occupied (render thread is 3 frames behind),
		// this blocks until a slot frees up -- natural backpressure.
		m_channel.write(std::move(packet));
	}

	void RenderThread::WaitIdle()
	{
		{
			std::lock_guard lock(m_reloadMutex);
			if (!m_shutdown)
			{
				m_shutdown = true;
			}
		}
		m_reloadCv.notify_one();
		m_channel.close();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SetReloadInProgress(bool inProgress)
	{
		m_reloadInProgress.store(inProgress, std::memory_order_release);
		if (!inProgress)
		{
			m_reloadCv.notify_one();
		}
	}

	bool RenderThread::IsReloadInProgress() const
	{
		return m_reloadInProgress.load(std::memory_order_acquire);
	}

	void RenderThread::ConfigureThisThread()
	{
#if defined(_WIN32)
		// Boost the render thread to time-critical priority so that kernel
		// DPCs, ISRs, and other system threads are far less likely to preempt
		// it. This is the primary mitigation for intermittent GPU-backpressure
		// stutter caused by Windows scheduler preemption.
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

		// Name the thread for debugger / ETW / crash-dump visibility.
		// SetThreadDescription is available since Windows 10 1607.
		SetThreadDescription(GetCurrentThread(), L"AetherCore RenderThread");
#elif defined(__linux__)
		// Name the thread (limited to 16 bytes including null-terminator).
		pthread_setname_np(pthread_self(), "Aether-Render");
#endif
	}

	void RenderThread::ThreadLoop()
	{
		AE_PROFILE_THREAD("RenderThread");

		ConfigureThisThread();

		while (!m_shutdown)
		{
			// Check reload flag BEFORE reading new frame.
			// This ensures we don't start a new frame while reload is in progress.
			if (IsReloadInProgress())
			{
				// Wait for any in-flight frame to complete before going idle.
				// This prevents threading errors when main thread calls WaitIdle().
				if (!m_isIdle.load(std::memory_order_acquire))
				{
					m_engine->WaitIdle();
					m_isIdle.store(true, std::memory_order_release);
				}

				// Block until reload completes instead of busy-waiting.
				// The condition variable avoids wasting CPU and provides
				// immediate wakeup when SetReloadInProgress(false) is called.
				{
					std::unique_lock lock(m_reloadMutex);
					m_reloadCv.wait(lock, [this] { return !IsReloadInProgress() || m_shutdown; });
				}
			}

			m_isIdle.store(false, std::memory_order_release);

			RenderFramePacket packet;

			try
			{
				packet = m_channel.read();
			}
			catch (const std::runtime_error&)
			{
				break;
			}

			// Execute the frame. This blocks on the GPU fence internally and
			// includes all command recording and submission.
			try
			{
				m_engine->ExecuteRenderFrame(packet);
			}
			catch (const std::exception& e)
			{
				AE_ERROR(LogCategory::Render, "RenderThread: ExecuteRenderFrame failed: {}", e.what());
				{
					std::lock_guard lock(m_reloadMutex);
					m_shutdown = true;
				}
				m_reloadCv.notify_one();
				m_channel.close();
				break;
			}

			// Publish the completed frame index (for statistics / shutdown).
			m_lastCompletedFrameIndex.store(packet.frameIndex, std::memory_order_release);
		}
	}

} // namespace aether
