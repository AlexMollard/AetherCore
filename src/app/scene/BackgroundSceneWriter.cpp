#include "scene/BackgroundSceneWriter.hpp"

#include <utility>

namespace aether::app::scene
{
	BackgroundSceneWriter::BackgroundSceneWriter()
	      : m_thread([this]() { Run(); })
	{
	}

	BackgroundSceneWriter::~BackgroundSceneWriter()
	{
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_wake.notify_all();
		if (m_thread.joinable())
		{
			// The worker drains all pending jobs before honouring stop, so a
			// last-moment Ctrl+S still reaches disk during shutdown.
			m_thread.join();
		}
	}

	void BackgroundSceneWriter::RequestSave(std::string sceneName, SceneDescription desc)
	{
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			m_pending[std::move(sceneName)] = std::move(desc); // coalesce: latest wins
		}
		m_wake.notify_one();
	}

	void BackgroundSceneWriter::Flush()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_drained.wait(lock, [this]() { return m_pending.empty() && !m_busy; });
	}

	void BackgroundSceneWriter::Run()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;)
		{
			m_wake.wait(lock, [this]() { return m_stop || !m_pending.empty(); });

			if (m_pending.empty())
			{
				// Only reached when stop was requested with nothing left to write.
				return;
			}

			const auto job = m_pending.begin();
			const std::string name = job->first;
			SceneDescription desc = std::move(job->second);
			m_pending.erase(job);
			m_busy = true;

			lock.unlock();
			SaveSceneFile(name, desc); // heavy work off the lock and off the main thread
			lock.lock();

			m_busy = false;
			if (m_pending.empty())
			{
				m_drained.notify_all();
			}
		}
	}
} // namespace aether::app::scene
