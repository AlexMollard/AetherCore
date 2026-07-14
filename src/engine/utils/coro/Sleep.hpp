#pragma once

#include <chrono>
#include <coroutine>
#include <thread>

#include "Executor.hpp"

namespace aether::coro
{

	// Uses a dedicated timer thread.  For high-frequency game-loop pacing prefer
	struct sleep_for
	{
		using Clock = std::chrono::steady_clock;
		Clock::duration dur;

		explicit sleep_for(Clock::duration d) noexcept
		      : dur(d)
		{
		}

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
