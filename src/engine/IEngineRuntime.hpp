#pragma once

#include <functional>

namespace aether
{
	// How RunExclusive treats frames already submitted to the render thread when
	// it parks the pipeline for a structural mutation.
	//
	//   Drain   - wait for every in-flight frame to finish rendering FIRST. Use
	//             when those frames reference resources that stay valid during the
	//             mutation (e.g. swapchain/viewport recreate: the frames must
	//             complete against the still-alive old targets before teardown).
	//
	//   Discard - do NOT drain; the render thread drops queued frames as it parks.
	//             Use when the mutation frees resources those in-flight frames
	//             reference (e.g. scene hot-reload frees entity/mesh buffers, so
	//             rendering the in-flight frames would touch freed memory).
	enum class QuiesceMode
	{
		Drain,
		Discard,
	};

	// Narrow engine-runtime surface exposed to the application (via the service
	// container) so app code can perform a structural mutation of GPU-referenced
	// state safely, without touching the render thread directly. Keeps the render
	// thread and frame pipeline engine-private.
	class IEngineRuntime
	{
	public:
		virtual ~IEngineRuntime() = default;

		// Quiesce the frame pipeline (optionally drain, park the render thread,
		// wait the GPU idle), invoke `mutation` single-threaded, then resume. The
		// mutation owns any scene/queue-specific cleanup (queue clears, entity
		// destruction); RunExclusive only provides the safe exclusive window.
		virtual void RunExclusive(QuiesceMode mode, std::function<void()> mutation) = 0;
	};
} // namespace aether
