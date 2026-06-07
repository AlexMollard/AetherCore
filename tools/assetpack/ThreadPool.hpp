#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Lightweight thread pool with task submission and result collection via futures.
// Tasks are executed by a fixed number of worker threads.
class ThreadPool
{
public:
	explicit ThreadPool(size_t numThreads = std::thread::hardware_concurrency())
	      : m_stop(false)
	{
		if (numThreads == 0)
		{
			numThreads = 1;
		}
		m_workers.reserve(numThreads);
		for (size_t i = 0; i < numThreads; ++i)
		{
			m_workers.emplace_back([this] { workerLoop(); });
		}
	}

	~ThreadPool()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		for (auto& w: m_workers)
		{
			w.join();
		}
	}

	template<typename F>
	auto submit(F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>>
	{
		using return_type = std::invoke_result_t<std::decay_t<F>>;
		auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));
		std::future<return_type> res = task->get_future();
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stop)
			{
				throw std::runtime_error("submit on stopped ThreadPool");
			}
			m_tasks.emplace([task] { (*task)(); });
		}
		m_cv.notify_one();
		return res;
	}

private:
	void workerLoop()
	{
		while (true)
		{
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
				if (m_stop && m_tasks.empty())
				{
					return;
				}
				task = std::move(m_tasks.front());
				m_tasks.pop();
			}
			task();
		}
	}

	std::vector<std::thread> m_workers;
	std::queue<std::function<void()>> m_tasks;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_stop;
};
