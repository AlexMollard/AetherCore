#include "rendering/RenderThread.hpp"

#include "AetherCore.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	RenderThread::RenderThread()
	      : m_channel(2) // double-buffered: game thread writes one slot while
	                     // render thread reads the other
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

		// Unblock the game thread if it's waiting on the consumed ack
		// (should not normally happen since Stop is called after the
		// game loop exits, but guard against edge cases).
		{
			std::lock_guard lock(m_ackMutex);
			m_consumed = true;
		}
		m_ackCv.notify_one();

		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SubmitFrame(RenderFramePacket packet)
	{
		AE_PROFILE_ZONE_N("RenderThread::Submit");

		// Block until a slot is available in the channel.
		m_channel.write(std::move(packet));

		// Wait for the render thread to consume the packet before returning.
		// This prevents the game thread from calling BeginFrame() (which runs
		// ImGui's UpdateTexturesNewFrame) while the render thread is still
		// processing texture uploads from the current frame.
		{
			std::unique_lock lock(m_ackMutex);
			m_ackCv.wait(lock, [this] { return m_consumed; });
			m_consumed = false;
		}
	}

	void RenderThread::WaitIdle()
	{
		// The channel is empty when the render thread has consumed everything.
		// However, the render thread may still be executing the last frame.
		// Close the channel so the render thread's read() returns an error,
		// then join the thread to wait for full completion.
		// (Callers should call Stop() first; this is a safety fallback.)
		if (!m_shutdown)
		{
			m_shutdown = true;
			m_channel.close();
			{
				std::lock_guard lock(m_ackMutex);
				m_consumed = true;
			}
			m_ackCv.notify_one();
		}
	}

	void RenderThread::ThreadLoop()
	{
		AE_PROFILE_THREAD("RenderThread");

		while (!m_shutdown)
		{
			RenderFramePacket packet;

			try
			{
				packet = m_channel.read();
			}
			catch (const std::runtime_error&)
			{
				// Channel closed - time to shut down.
				break;
			}

			// Signal the game thread that we've consumed the packet.
			// This unblocks SubmitFrame BEFORE the GPU fence wait, so the
			// game thread can start BeginFrame() while we wait for the GPU.
			{
				std::lock_guard lock(m_ackMutex);
				m_consumed = true;
			}
			m_ackCv.notify_one();

			// Execute the frame.  This blocks on the GPU fence internally.
			m_engine->ExecuteRenderFrame(packet);
		}
	}

} // namespace aether
