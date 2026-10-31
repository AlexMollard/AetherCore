#pragma once

#include <functional>

namespace aether
{
	// How RunExclusive treats frames already submitted to the render thread when
	enum class QuiesceMode
	{
		Drain,
		Discard,
	};

	// state safely, without touching the render thread directly. Keeps the render
	class IEngineRuntime
	{
	public:
		virtual ~IEngineRuntime() = default;

		// Quiesce the frame pipeline (optionally drain, park the render thread,
		// wait for GPU idle). A throwing mutation must leave published resources usable;
		// rendering resumes on unwind. Irrecoverable mutations must terminate instead
		// of throwing into a recoverable script/export barrier.
		virtual void RunExclusive(QuiesceMode mode, std::function<void()> mutation) = 0;

		// Real (wall-clock) seconds since the frame loop started, unaffected by the play-speed /
		// pause time scale. Use this to animate UI while the game is frozen (e.g. a pause menu),
		// where the scaled game clock (Time.TotalTime) stops advancing.
		[[nodiscard]] virtual double RealElapsedSeconds() const = 0;
	};
} // namespace aether
