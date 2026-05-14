#pragma once

#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "FileRequest.hpp"
#include "utils/coro/Executor.hpp"
#include "utils/coro/Task.hpp"

namespace aether::io
{
	// -----------------------------------------------------------------------
	// IoExecutor - dedicated background I/O thread that runs both plain jobs
	// (for backward compatibility) and coroutine tasks.  It also implements
	// the coro::executor interface so coroutines can be scheduled onto the
	// I/O thread directly.
	// -----------------------------------------------------------------------
	class IoExecutor final : public coro::executor
	{
	public:
		IoExecutor();
		~IoExecutor() override;

		IoExecutor(const IoExecutor&) = delete;
		IoExecutor& operator=(const IoExecutor&) = delete;

		// ---- coro::executor -----------------------------------------------
		void schedule(std::coroutine_handle<> h) override;

		const char* name() const noexcept override
		{
			return "IoExecutor";
		}

		// ---- Job-based API (backward-compatible) ---------------------------

		// Submit a plain callable with a priority level.
		void Submit(IOPriority priority, std::function<void()> job);

		// Blocks until all submitted jobs AND scheduled coroutines have
		// completed.
		void Flush();

		// Convenience: wrap a job as a coroutine task.
		template<typename F>
		coro::task<std::invoke_result_t<F>> Run([[maybe_unused]] IOPriority priority, F&& fn)
		{
			co_return std::invoke(std::forward<F>(fn));
		}

	private:
		struct Job
		{
			IOPriority priority;
			std::function<void()> work;

			bool operator<(const Job& other) const noexcept
			{
				return static_cast<int>(priority) > static_cast<int>(other.priority);
			}
		};

		void WorkerLoop();

		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_workCv;
		std::condition_variable m_idleCv;

		std::priority_queue<Job> m_queue;
		std::vector<std::coroutine_handle<>> m_coroQueue;
		std::size_t m_pendingCount = 0;
		bool m_shutdown = false;
	};

} // namespace aether::io
