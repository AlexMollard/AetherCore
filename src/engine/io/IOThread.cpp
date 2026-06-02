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
			std::scoped_lock lock(m_mutex);
			m_shutdown = true;
		}
		m_workCv.notify_one();
		m_thread.join();
	}

	void IoExecutor::Submit(IOPriority priority, std::function<void()> job)
	{
		{
			std::scoped_lock lock(m_mutex);
			++m_pendingCount;
			m_queue.push({priority, std::move(job)});
		}
		m_workCv.notify_one();
	}

	void IoExecutor::schedule(std::coroutine_handle<> h)
	{
		{
			std::scoped_lock lock(m_mutex);
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
			// --- Dequeue one unit of work (job or coroutine) ----------------
			std::function<void()> work;
			std::coroutine_handle<> coro = nullptr;

			{
				std::unique_lock lock(m_mutex);
				m_workCv.wait(lock, [this] { return (!m_queue.empty() || !m_coroQueue.empty()) || m_shutdown; });

				if (m_shutdown && m_queue.empty() && m_coroQueue.empty())
				{
					return;
				}

				// Prefer coroutine handles over jobs (coroutines are typically
				// higher-value cancellation boundaries).  Within each category
				// we respect priority order.
				if (!m_coroQueue.empty())
				{
					coro = m_coroQueue.back();
					m_coroQueue.pop_back();
				}
				else
				{
					work = std::move(m_queue.top().work);
					m_queue.pop();
				}
			}

			// --- Execute ---------------------------------------------------
			if (coro)
			{
				AE_PROFILE_ZONE_N("IO::CoroResume");
				coro.resume();
			}
			else
			{
				AE_PROFILE_ZONE_N("IO::Job");
				work();
			}

			// --- Decrement pending count and notify idle waiters -----------
			{
				std::scoped_lock lock(m_mutex);
				--m_pendingCount;
			}
			m_idleCv.notify_all();
		}
	}
} // namespace aether::io
