#include "RenderThread.hpp"

#include "AetherCore.hpp"
#include "Profiler.hpp"

namespace aether
{
	void RenderThread::Start(AetherCore& engine)
	{
		m_engine = &engine;
		m_thread = std::thread([this] { ThreadLoop(); });
	}

	void RenderThread::Stop()
	{
		{
			std::unique_lock lock(m_mutex);
			m_shutdown = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
		{
			m_thread.join();
		}
	}

	void RenderThread::SubmitFrame(RenderFramePacket packet)
	{
		AE_PROFILE_ZONE_N("RenderThread::Submit");

		{
			std::unique_lock lock(m_mutex);
			// Wait for the render thread to be fully idle before publishing the next
			// packet.  We need both conditions:
			//   - !m_hasFrame: no queued packet waiting to be picked up
			//   - !m_executing: previous frame is no longer being executed
			// This prevents the game thread from reusing/clearing a draw slot while
			// the render thread may still be reading it.
			m_cv.wait(lock, [this] { return !m_hasFrame && !m_executing; });

			m_packet = std::move(packet);
			m_hasFrame = true;
			m_consumed = false;
		}
		m_cv.notify_one(); // wake the render thread

		// Wait for the render thread to pick up the packet ("consumed").
		// This is just thread scheduling latency — the render thread signals consumed
		// immediately on waking, before doing any fence wait or recording.
		{
			std::unique_lock lock(m_mutex);
			m_cv.wait(lock, [this] { return m_consumed || m_shutdown; });
		}
	}

	void RenderThread::WaitIdle()
	{
		std::unique_lock lock(m_mutex);
		m_cv.wait(lock, [this] { return !m_hasFrame && !m_executing; });
	}

	void RenderThread::ThreadLoop()
	{
		AE_PROFILE_THREAD("RenderThread");

		while (true)
		{
			RenderFramePacket packet;

			{
				std::unique_lock lock(m_mutex);
				m_cv.wait(lock, [this] { return m_hasFrame || m_shutdown; });

				if (m_shutdown && !m_hasFrame)
				{
					break;
				}

				packet = m_packet;
				m_hasFrame = false;
				m_consumed = true;
				m_executing = true;
			}
			// Unblock the game thread as early as possible — before the fence wait.
			// The game thread can now start the next simulation frame while the render
			// thread blocks on the GPU fence in the background.
			m_cv.notify_all();

			m_engine->ExecuteRenderFrame(packet);

			{
				std::unique_lock lock(m_mutex);
				m_executing = false;
			}
			m_cv.notify_all();
		}
	}

} // namespace aether
