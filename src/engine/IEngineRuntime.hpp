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
	};
} // namespace aether
