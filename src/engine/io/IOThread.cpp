#include "IOThread.hpp"

#include "utils/Profiler.hpp"

namespace aether::io
{
	IOThread::IOThread()
	      : m_thread(&IOThread::WorkerLoop, this)
	{
	}

	IOThread::~IOThread()
	{
		{
			std::scoped_lock lock(m_mutex);
			m_shutdown = true;
		}
		m_workCv.notify_one();
		m_thread.join();
	}

	void IOThread::Submit(IOPriority priority, std::function<void()> job)
	{
		{
			std::scoped_lock lock(m_mutex);
			++m_pendingCount;
			m_queue.push({ priority, std::move(job) });
		}
		m_workCv.notify_one();
	}

	void IOThread::Flush()
	{
		std::unique_lock lock(m_mutex);
		m_idleCv.wait(lock, [this] { return m_pendingCount == 0 || m_shutdown; });
	}

	void IOThread::WorkerLoop()
	{
		AE_PROFILE_THREAD("IOThread");
		while (true)
		{
			std::function<void()> work;

			{
				std::unique_lock lock(m_mutex);
				m_workCv.wait(lock, [this] { return !m_queue.empty() || m_shutdown; });

				if (m_shutdown && m_queue.empty())
				{
					return;
				}

				work = std::move(m_queue.top().work);
				m_queue.pop();
			}

			{
				AE_PROFILE_ZONE_N("IO::Job");
				work();
			}

			{
				std::scoped_lock lock(m_mutex);
				--m_pendingCount;
			}
			m_idleCv.notify_all();
		}
	}
} // namespace aether::io
