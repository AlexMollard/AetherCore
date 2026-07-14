#pragma once

#include <cstdint>

namespace aether
{
	// WITHOUT owning the frame pipeline, render thread, or resource lifecycle.
	class EngineClient
	{
	public:
		virtual ~EngineClient() = default;

		virtual void OnFrameBegin()
		{
		}

		virtual double GetTimeScale()
		{
			return 1.0;
		}

		// Per-frame simulation on the main thread. gameDt is the scaled dt.
		virtual void OnUpdate(double gameDt, std::uint64_t frameIndex) = 0;

		// Per-frame UI build on the main thread, between ImGui BeginFrame and the
		virtual void OnBuildUI(double gameDt, std::uint64_t frameIndex) = 0;

		// texture descriptors). Runs on the main thread with the render thread
		virtual void OnRenderTargetsInvalidated()
		{
		}
	};
} // namespace aether
