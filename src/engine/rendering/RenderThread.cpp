#include "rendering/RenderThread.hpp"

#include "AetherCore.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/Swapchain.hpp"

// Platform thread configuration
#if defined(_WIN32)
#	include <windows.h>
#elif defined(__linux__)
#	include <pthread.h>
#endif

namespace aether
{
	RenderThread::RenderThread()
	      : m_channel(Swapchain::kMaxFramesInFlight) // match swapchain depth: game thread can write
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
			const std::lock_guard lock(m_reloadMutex);
			m_shutdown = true;
		}
		m_reloadCv.notify_one();
		m_completionCv.notify_all();
		m_channel.close();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SubmitFrame(RenderFramePacket packet)
	{
		AE_PROFILE_ZONE();

		// The render thread picks up the packet asynchronously.
		m_channel.write(std::move(packet));
	}

	void RenderThread::WaitUntilFrameCompleted(const std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		std::unique_lock lock(m_completionMutex);
		m_completionCv.wait(lock,
		        [this, frameIndex]
		        {
			        const std::uint64_t completed = m_lastCompletedFrameIndex.load(std::memory_order_acquire);
			        return m_shutdown.load(std::memory_order_acquire) || (completed != std::numeric_limits<std::uint64_t>::max() && completed >= frameIndex);
		        });
	}

	void RenderThread::WaitIdle()
	{
		{
			const std::lock_guard lock(m_reloadMutex);
			if (!m_shutdown)
			{
				m_shutdown = true;
			}
		}
		m_reloadCv.notify_one();
		m_completionCv.notify_all();
		m_channel.close();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SetReloadInProgress(bool inProgress)
	{
		m_reloadInProgress.store(inProgress, std::memory_order_release);
		m_reloadCv.notify_all();
	}

	void RenderThread::WaitPaused()
	{
		std::unique_lock lock(m_reloadMutex);
		m_reloadCv.wait(lock, [this] { return m_shutdown || m_isIdle.load(std::memory_order_acquire); });
	}

	bool RenderThread::IsReloadInProgress() const
	{
		return m_reloadInProgress.load(std::memory_order_acquire);
	}

	void RenderThread::ConfigureThisThread()
	{
#if defined(_WIN32)
		// Boost the render thread to time-critical priority so that kernel
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

		// Name the thread for debugger / ETW / crash-dump visibility.
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
			if (IsReloadInProgress())
			{
				// This prevents threading errors when main thread calls WaitIdle().
				if (!m_isIdle.load(std::memory_order_acquire))
				{
					m_engine->WaitIdle();
					m_isIdle.store(true, std::memory_order_release);
					m_reloadCv.notify_all();
				}

				{
					std::unique_lock lock(m_reloadMutex);
					m_reloadCv.wait(lock, [this] { return !IsReloadInProgress() || m_shutdown; });
				}
			}

			RenderFramePacket packet;

			try
			{
				m_isIdle.store(true, std::memory_order_release);
				m_reloadCv.notify_all();
				packet = m_channel.read();
				if (IsReloadInProgress())
				{
					m_engine->DiscardPendingFrameQueues(packet);
					m_engine->RecycleUiOverlayFrameData(std::move(packet.uiOverlay));
					m_isIdle.store(true, std::memory_order_release);
					m_reloadCv.notify_all();
					continue;
				}
				m_isIdle.store(false, std::memory_order_release);
			}
			catch (const std::runtime_error&)
			{
				break;
			}

			try
			{
				m_engine->ExecuteRenderFrame(packet);
			}
			catch (const std::exception& e)
			{
				AE_ERROR(LogCategory::Render, "RenderThread: ExecuteRenderFrame failed: {}", e.what());
				m_engine->RecycleUiOverlayFrameData(std::move(packet.uiOverlay));
				m_engine->DiscardAllPendingFrameQueues();
				{
					const std::lock_guard lock(m_reloadMutex);
					m_shutdown = true;
				}
				m_reloadCv.notify_one();
				m_completionCv.notify_all();
				m_channel.close();
				break;
			}

			m_engine->RecycleUiOverlayFrameData(std::move(packet.uiOverlay));

			m_lastCompletedFrameIndex.store(packet.frameIndex, std::memory_order_release);
			m_completionCv.notify_all();
			m_isIdle.store(true, std::memory_order_release);
			m_reloadCv.notify_all();
		}
	}

} // namespace aether
