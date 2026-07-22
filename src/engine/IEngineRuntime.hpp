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
		virtual void RunExclusive(QuiesceMode mode, std::function<void()> mutation) = 0;

		// Real (wall-clock) seconds since the frame loop started, unaffected by the play-speed /
		// pause time scale. Use this to animate UI while the game is frozen (e.g. a pause menu),
		// where the scaled game clock (Time.TotalTime) stops advancing.
		[[nodiscard]] virtual double RealElapsedSeconds() const = 0;
	};
} // namespace aether
