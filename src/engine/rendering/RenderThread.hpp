#pragma once

#include <atomic>
#include <cstdint>
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
		std::uint32_t drawSlot = 0;

		// Elapsed simulation time in seconds (for time-based shader effects).
		float elapsedTime = 0.0f;
	};

	// Dedicated render thread that owns all Vulkan submission work.
	//
	// Frame pipeline (fully pipelined):
	//   Game thread   : Sim N -> SubmitFrame(N) -> Sim N+1 -> SubmitFrame(N+1) -> ...
	//   Render thread :              Read N -> Exec N            Read N+1 -> Exec N+1 -> ...
	//
	// Synchronisation uses a bounded channel (capacity = 2 = double-buffered).
	// SubmitFrame writes to the channel and returns immediately -- the game thread
	// continues with frame N+1's simulation while the render thread executes frame N.
	//
	// All shared render data is deep-copied per frame slot, so there are no
	// data races between the game and render threads.
	class RenderThread
	{
	public:
		RenderThread();

		void Start(AetherCore& engine);
		void Stop();

		// Hand off a completed render packet to the render thread.
		// Returns immediately (microsecond latency).  The render thread picks
		// up the packet from the channel and processes it asynchronously.
		void SubmitFrame(RenderFramePacket packet);

		// Block until the render thread has no pending work.
		// Call before tearing down Vulkan resources.
		void WaitIdle();

	private:
		void ThreadLoop();

		AetherCore* m_engine = nullptr;
		std::thread m_thread;
		coro::channel<RenderFramePacket> m_channel;

		// Tracks the index of the last fully-executed frame (for shutdown /
		// debugging / statistics).  Not used for per-frame synchronisation.
		std::atomic<std::uint64_t> m_lastCompletedFrameIndex{ 0 };
		std::atomic<bool> m_shutdown{ false };
	};

} // namespace aether
