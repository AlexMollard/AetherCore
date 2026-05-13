#pragma once

#include <chrono>
#include <coroutine>
#include <thread>

#include "Executor.hpp"

namespace aether::coro
{

	// ---------------------------------------------------------------------------
	// awaitable sleep_for — suspends the current coroutine for at least the given
	// duration, then resumes via the default executor.
	//
	// Uses a dedicated timer thread.  For high-frequency game-loop pacing prefer
	// the existing FramePacer::Wait() on the game thread; this is intended for
	// slower operations (e.g. "wait 2 seconds before retrying a file load").
	// ---------------------------------------------------------------------------
	struct sleep_for
	{
		using Clock = std::chrono::steady_clock;
		Clock::duration dur;

		explicit sleep_for(Clock::duration d) noexcept
		      : dur(d)
		{
		}

		// std::chrono literal convenience
		template<typename Rep, typename Period>
		explicit sleep_for(const std::chrono::duration<Rep, Period>& d) noexcept
		      : dur(std::chrono::duration_cast<Clock::duration>(d))
		{
		}

		bool await_ready() const noexcept
		{
			return dur <= Clock::duration::zero();
		}

		void await_suspend(std::coroutine_handle<> awaiting) noexcept
		{
			// Launch a detached timer thread that sleeps then resumes the coroutine.
			// In a production engine this would be replaced by a tick-based timer
			// wheel to avoid thread churn.
			std::thread(
			        [dur = this->dur, awaiting]()
			        {
				        std::this_thread::sleep_for(dur);
				        executor::schedule_on_resumer(awaiting);
			        })
			        .detach();
		}

		void await_resume() noexcept
		{
		}
	};

} // namespace aether::coro
