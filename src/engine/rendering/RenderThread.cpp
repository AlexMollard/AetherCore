#include "rendering/RenderThread.hpp"

#include "AetherCore.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/Swapchain.hpp"

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
		m_shutdown = true;
		m_channel.close();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SubmitFrame(RenderFramePacket packet)
	{
		AE_PROFILE_ZONE_N("RenderThread::Submit");

		// Write to the channel and return immediately.
		// The render thread picks up the packet asynchronously.
		// If both channel slots are occupied (render thread is 2 frames behind),
		// this blocks until a slot frees up -- natural backpressure.
		m_channel.write(std::move(packet));
	}

	void RenderThread::WaitIdle()
	{
		if (!m_shutdown)
		{
			m_shutdown = true;
			m_channel.close();
		}
		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SetReloadInProgress(bool inProgress)
	{
		m_reloadInProgress.store(inProgress, std::memory_order_release);
	}

	bool RenderThread::IsReloadInProgress() const
	{
		return m_reloadInProgress.load(std::memory_order_acquire);
	}

	void RenderThread::ThreadLoop()
	{
		AE_PROFILE_THREAD("RenderThread");

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

				while (IsReloadInProgress())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
				m_shutdown = true;
				m_channel.close();
				break;
			}

			// Publish the completed frame index (for statistics / shutdown).
			m_lastCompletedFrameIndex.store(packet.frameIndex, std::memory_order_release);
		}
	}

} // namespace aether
