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
		// Close the channel to unblock any pending read, then join.
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
				break;
			}

			// Execute the frame. This blocks on the GPU fence internally and
			// includes all command recording and submission.
			m_engine->ExecuteRenderFrame(packet);

			// Publish the completed frame index (for statistics / shutdown).
			m_lastCompletedFrameIndex.store(packet.frameIndex, std::memory_order_release);
		}
	}

} // namespace aether
