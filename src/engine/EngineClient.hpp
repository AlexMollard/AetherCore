#pragma once

#include <cstdint>

namespace aether
{
	// Hook interface the engine's frame loop (AetherCore::RunFrameLoop) calls back
	// into so the application can inject game logic, UI, and lifecycle handling
	// WITHOUT owning the frame pipeline, render thread, or resource lifecycle.
	//
	// The engine owns the loop; the app implements these hooks. Deliberately free
	// of any app types (e.g. LayerContext) so the engine has zero dependency on the
	// application layer - the app reconstructs whatever context it needs from the
	// primitives passed here.
	class EngineClient
	{
	public:
		virtual ~EngineClient() = default;

		// Top of frame, before events are pumped. Intended for draining the
		// app-owned coroutine executor.
		virtual void OnFrameBegin()
		{
		}

		// Game-time multiplier polled AFTER input is updated (so debug fast-forward
		// keys read fresh state). Return 1.0 for real-time. Applied to the raw
		// inter-frame dt to produce the game dt used for simulation.
		virtual double GetTimeScale()
		{
			return 1.0;
		}

		// Per-frame simulation on the main thread. gameDt is the scaled dt.
		virtual void OnUpdate(double gameDt, std::uint64_t frameIndex) = 0;

		// Per-frame UI build on the main thread, between ImGui BeginFrame and the
		// draw-data capture. gameDt is provided because panels surface it (e.g. the
		// performance graph reflects fast-forward, matching pre-refactor behaviour).
		virtual void OnBuildUI(double gameDt, std::uint64_t frameIndex) = 0;

		// Broadcast after swapchain / scene-viewport render targets were destroyed
		// and recreated, so the app can drop retained GPU references (e.g. ImGui
		// texture descriptors). Runs on the main thread with the render thread
		// parked and the GPU idle.
		virtual void OnRenderTargetsInvalidated()
		{
		}
	};
} // namespace aether
