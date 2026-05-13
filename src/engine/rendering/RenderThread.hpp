#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <glm/glm.hpp>

#include "utils/coro/Channel.hpp"

namespace aether
{
	class AetherCore;

	// Per-frame render data snapshot produced by the game thread and consumed by
	// the render thread. All fields are captured from game state BEFORE the render
	// thread starts executing, so there are no races with the next simulation tick.
	struct RenderFramePacket
	{
		// Camera matrices snapshotted at end of simulation.
		glm::mat4 view{ 1.0f };
		glm::mat4 proj{ 1.0f };
		glm::vec4 cameraWorldPos{ 0.0f };
		bool hasCameraData = false;

		// Lighting state snapshotted at end of simulation.
		glm::vec4 sunDirectionIntensity{ 0.0f, -1.0f, 0.0f, 1.0f };
		glm::vec4 ambientColor{ 0.2f, 0.2f, 0.2f, 1.0f };
		glm::vec4 sunColor{ 1.0f };
		glm::vec4 skyHorizonColor{ 1.0f };
		glm::vec4 skyZenithColor{ 0.5f, 0.7f, 1.0f, 1.0f };
		glm::vec4 skyVoidColor{ 0.0f };

		// Stable GPU resource addresses.
		std::uint64_t materialBufferAddr = 0;

		// Frame identity - render thread uses these for GPU buffer slot selection.
		std::uint64_t frameIndex = 0;
		// CPU-side double-buffer slot (frameIndex % kMaxFramesInFlight).
		// The game thread wrote draw commands and UI lists into this slot.
		// The render thread reads from the same slot while the game thread
		// writes to the other slot for the next frame.
		std::uint32_t drawSlot = 0;
	};

	// Dedicated render thread that owns all Vulkan submission work.
	//
	// Frame pipeline:
	//   Game thread  : Sim N -> SetWriteSlot(N) -> LayerGui -> GatherDraws -> PreparePacket -> SubmitFrame(N)
	//   Render thread:                                                                   <- wake, read ->
	//                                                                                      WaitFence -> Record -> Submit -> Present
	//
	// Synchronisation uses a bounded channel (capacity = 2 = double-buffered)
	// plus a consumed-acknowledgment condvar.
	//
	// The game thread blocks at SubmitFrame only until the render thread has
	// consumed the packet from the channel (microsecond latency).  The render
	// thread reads from the channel BEFORE the fence wait, so the game thread
	// is unblocked before GPU work begins.
	//
	// The consumed-acknowledgment prevents the game thread from starting
	// the next frame's BeginFrame() — which may call ImGui's texture update
	// checks — while the render thread is still processing texture uploads
	// from the previous frame.
	class RenderThread
	{
	public:
		RenderThread();

		void Start(AetherCore& engine);
		void Stop();

		// Hand off a completed render packet to the render thread.
		// Blocks until the render thread has consumed the packet
		// (microsecond latency, not GPU-frame latency).
		void SubmitFrame(RenderFramePacket packet);

		// Block until the render thread has no pending work.
		// Call before tearing down Vulkan resources.
		void WaitIdle();

	private:
		void ThreadLoop();

		AetherCore* m_engine = nullptr;
		std::thread m_thread;
		coro::channel<RenderFramePacket> m_channel;

		// Consumed-acknowledgment: protects game-thread operations that must
		// not run concurrently with the render thread's ExecuteRenderFrame
		// (e.g. ImGui's UpdateTexturesNewFrame check during BeginFrame).
		std::mutex m_ackMutex;
		std::condition_variable m_ackCv;
		bool m_consumed = false;
		bool m_shutdown = false;
	};

} // namespace aether
