#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "FileRequest.hpp"

namespace meow::io
{
	class IOThread
	{
	public:
		IOThread();
		~IOThread();

		IOThread(const IOThread&) = delete;
		IOThread& operator=(const IOThread&) = delete;

		void Submit(IOPriority priority, std::function<void()> job);

		// Blocks until all submitted jobs have completed.
		void Flush();

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
		std::size_t m_pendingCount = 0;
		bool m_shutdown = false;
	};
}
