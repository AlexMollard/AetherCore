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
		m_state.Shutdown();
		m_channel.close();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
		// Stop may leave unread packets in the closed channel. Release their
		// streamed asset leases while the engine and asset registries still live.
		while (auto packet = m_channel.try_read_for(std::chrono::milliseconds(0)))
		{
			m_engine->DeferGrassStreaming(*packet);
			m_engine->DiscardPendingFrameQueues(*packet);
			m_engine->RecycleUiOverlayFrameData(std::move(packet->uiOverlay));
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
		m_state.WaitUntilFrameCompleted(frameIndex);
	}

	void RenderThread::WaitIdle()
	{
		Stop();
	}

	void RenderThread::SetReloadInProgress(bool inProgress)
	{
		m_state.SetReloadInProgress(inProgress);
	}

	void RenderThread::WaitPaused()
	{
		m_state.WaitPaused();
	}

	bool RenderThread::IsReloadInProgress() const
	{
		return m_state.IsReloadInProgress();
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

		while (!m_state.IsShutdown())
		{
			m_state.WaitForReload();
			if (m_state.IsShutdown())
			{
				break;
			}

			RenderFramePacket packet;

			try
			{
				packet = m_channel.read();
				if (!m_state.BeginFrame())
				{
					m_engine->DeferGrassStreaming(packet);
					m_engine->DiscardPendingFrameQueues(packet);
					m_engine->RecycleUiOverlayFrameData(std::move(packet.uiOverlay));
					continue;
				}
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
				m_state.Shutdown();
				m_channel.close();
				break;
			}

			m_engine->RecycleUiOverlayFrameData(std::move(packet.uiOverlay));

			m_state.CompleteFrame(packet.frameIndex);
		}
	}

} // namespace aether
