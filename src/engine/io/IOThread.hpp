#pragma once

#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "IOPriority.hpp"
#include "utils/coro/Executor.hpp"
#include "utils/coro/Task.hpp"

namespace aether::io
{
	// IoExecutor - dedicated background I/O thread that runs both plain jobs
	class IoExecutor final : public coro::executor
	{
	public:
		IoExecutor();
		~IoExecutor() override;

		IoExecutor(const IoExecutor&) = delete;
		IoExecutor& operator=(const IoExecutor&) = delete;

		void schedule(std::coroutine_handle<> h) override;

		[[nodiscard]] const char* name() const noexcept override
		{
			return "IoExecutor";
		}

		void Submit(IOPriority priority, std::function<void()> job);

		void Flush();

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
