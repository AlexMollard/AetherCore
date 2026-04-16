#pragma once

#include <condition_variable>
#include <cstdint>
#include <glm/glm.hpp>
#include <mutex>
#include <thread>
#include <vulkan/vulkan.h>

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
		VkDeviceAddress materialBufferAddr = 0;

		// Frame identity — render thread uses these for GPU buffer slot selection.
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
	//   Game thread  : Sim N → SetWriteSlot(N) → LayerGui → GatherDraws → PreparePacket → SubmitFrame(N)
	//   Render thread:                                                                   ← wake, "consumed" →
	//                                                                                      WaitFence → Record → Submit → Present
	//
	// The game thread blocks at SubmitFrame only until the render thread signals
	// "consumed" (it has picked up the packet), which happens before the fence wait.
	// This means the game thread is stalled for microseconds (thread wake latency)
	// rather than milliseconds (GPU frame time), giving the GPU the full sim frame
	// budget to finish before the render thread reaches the fence.
	class RenderThread
	{
	public:
		void Start(AetherCore& engine);
		void Stop();

		// Hand off a completed render packet to the render thread.
		// Blocks until:
		//   1. The render thread is idle (has consumed its previous packet), AND
		//   2. The render thread signals "consumed" for this new packet.
		// Condition 1 prevents the game thread from racing more than one frame ahead.
		// Condition 2 ensures the render thread has picked up this packet before the
		// game thread overwrites the double-buffered CPU state for the next frame.
		// Both waits are expected to be near-zero (thread scheduling latency only).
		void SubmitFrame(RenderFramePacket packet);

		// Block until the render thread has no pending work.
		// Call before tearing down Vulkan resources.
		void WaitIdle();

	private:
		void ThreadLoop();

		AetherCore* m_engine = nullptr;
		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_cv;

		RenderFramePacket m_packet{};
		bool m_hasFrame = false;  // render thread has a packet waiting
		bool m_consumed = false;  // render thread picked up the current packet
		bool m_executing = false; // render thread is executing a frame
		bool m_shutdown = false;
	};

} // namespace aether
