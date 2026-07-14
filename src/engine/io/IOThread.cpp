#include "IOThread.hpp"

#include "utils/Profiler.hpp"

namespace aether::io
{
	IoExecutor::IoExecutor()
	      : m_thread(&IoExecutor::WorkerLoop, this)
	{
	}

	IoExecutor::~IoExecutor()
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_shutdown = true;
		}
		m_workCv.notify_one();
		m_thread.join();
	}

	void IoExecutor::Submit(IOPriority priority, std::function<void()> job)
	{
		{
			const std::scoped_lock lock(m_mutex);
			++m_pendingCount;
			m_queue.push({.priority = priority, .work = std::move(job)});
		}
		m_workCv.notify_one();
	}

	void IoExecutor::schedule(std::coroutine_handle<> h)
	{
		{
			const std::scoped_lock lock(m_mutex);
			++m_pendingCount;
			m_coroQueue.push_back(h);
		}
		m_workCv.notify_one();
	}

	void IoExecutor::Flush()
	{
		std::unique_lock lock(m_mutex);
		m_idleCv.wait(lock, [this] { return m_pendingCount == 0 || m_shutdown; });
	}

	void IoExecutor::WorkerLoop()
	{
		AE_PROFILE_THREAD("IoExecutor");
		while (true)
		{
			std::function<void()> work;
			std::coroutine_handle<> coro = nullptr;

			{
				std::unique_lock lock(m_mutex);
				m_workCv.wait(lock, [this] { return (!m_queue.empty() || !m_coroQueue.empty()) || m_shutdown; });

				if (m_shutdown && m_queue.empty() && m_coroQueue.empty())
				{
					return;
				}

				if (!m_coroQueue.empty())
				{
					coro = m_coroQueue.back();
					m_coroQueue.pop_back();
				}
				else
				{
					work = m_queue.top().work;
					m_queue.pop();
				}
			}

			if (coro)
			{
				AE_PROFILE_ZONE();
				coro.resume();
			}
			else
			{
				AE_PROFILE_ZONE();
				work();
			}

			{
				const std::scoped_lock lock(m_mutex);
				--m_pendingCount;
			}
			m_idleCv.notify_all();
		}
	}
} // namespace aether::io
