#pragma once

#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>

namespace aether
{
	// All render-thread wait predicates share one mutex. A transition cannot
	// notify between a waiter's predicate check and registration with the CV.
	class RenderThreadState
	{
	public:
		void Shutdown()
		{
			{
				const std::lock_guard lock(m_mutex);
				m_shutdown = true;
			}
			m_changed.notify_all();
		}

		[[nodiscard]] bool IsShutdown() const
		{
			const std::lock_guard lock(m_mutex);
			return m_shutdown;
		}

		void SetReloadInProgress(bool inProgress)
		{
			{
				const std::lock_guard lock(m_mutex);
				m_reloadInProgress = inProgress;
			}
			m_changed.notify_all();
		}

		[[nodiscard]] bool IsReloadInProgress() const
		{
			const std::lock_guard lock(m_mutex);
			return m_reloadInProgress;
		}

		void WaitForReload()
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock, [this] { return !m_reloadInProgress || m_shutdown; });
		}

		void WaitPaused()
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock, [this] { return m_idle || m_shutdown; });
		}

		// Admission and the idle transition must be atomic with starting a reload.
		// Once WaitPaused returns, no newly dequeued frame can start rendering.
		[[nodiscard]] bool BeginFrame()
		{
			const std::lock_guard lock(m_mutex);
			if (m_reloadInProgress || m_shutdown)
			{
				return false;
			}
			m_idle = false;
			return true;
		}

		void CompleteFrame(std::uint64_t frameIndex)
		{
			{
				const std::lock_guard lock(m_mutex);
				m_lastCompletedFrameIndex = frameIndex;
				m_idle = true;
			}
			m_changed.notify_all();
		}

		void WaitUntilFrameCompleted(std::uint64_t frameIndex)
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock,
			        [this, frameIndex]
			        {
				        return m_shutdown || (m_lastCompletedFrameIndex != std::numeric_limits<std::uint64_t>::max() && m_lastCompletedFrameIndex >= frameIndex);
			        });
		}

	private:
		mutable std::mutex m_mutex;
		std::condition_variable m_changed;
		std::uint64_t m_lastCompletedFrameIndex = std::numeric_limits<std::uint64_t>::max();
		bool m_shutdown = false;
		bool m_reloadInProgress = false;
		bool m_idle = true;
	};
} // namespace aether
