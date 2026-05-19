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

		// Wait for the render thread to finish the full frame before returning.
		// This prevents the game thread from calling ImGui::NewFrame() (which
		// runs UpdateTexturesNewFrame) while the render thread is still inside
		// ImGui_ImplVulkan_UpdateTexture, where tex->TexID is set before
		// tex->Status is updated to OK - an invariant the assert checks.
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

			// Execute the frame. This blocks on the GPU fence internally and
			// includes ImGui texture uploads (RenderDrawData -> UpdateTexture).
			m_engine->ExecuteRenderFrame(packet);

			// Signal the game thread only after the full frame is done.
			// Unblocking earlier would let the game thread call ImGui::NewFrame()
			// while UpdateTexture still has tex->TexID set but Status != OK,
			// triggering the ImFontAtlasUpdateNewFrame assertion.
			{
				std::lock_guard lock(m_ackMutex);
				m_consumed = true;
			}
			m_ackCv.notify_one();
		}
	}

} // namespace aether
