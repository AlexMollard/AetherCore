#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "Executor.hpp"

namespace aether::coro
{

	// Internal locking makes it thread-safe for the general case; the "SPSC" in
	template<typename T>
	class channel
	{
	public:
		explicit channel(std::size_t capacity)
		      : m_capacity(capacity)
		{
		}

		channel(const channel&) = delete;
		channel& operator=(const channel&) = delete;
		channel(channel&&) = delete;
		channel& operator=(channel&&) = delete;
		~channel() = default;

		void write(T value)
		{
			std::unique_lock lock(m_mutex);
			m_notFull.wait(lock, [this] { return m_buffer.size() < m_capacity || m_closed; });
			if (m_closed)
			{
				return;
			}
			m_buffer.push_back(std::move(value));
			m_notEmpty.notify_one();
		}

		T read()
		{
			std::unique_lock lock(m_mutex);
			m_notEmpty.wait(lock, [this] { return !m_buffer.empty() || m_closed; });
			if (m_buffer.empty())
			{
				throw std::runtime_error("channel::read: closed with no data");
			}
			T value = std::move(m_buffer.front());
			m_buffer.pop_front();
			m_notFull.notify_one();
			return value;
		}

		template<typename Rep, typename Period>
		std::optional<T> try_read_for(const std::chrono::duration<Rep, Period>& timeout)
		{
			std::unique_lock lock(m_mutex);
			if (!m_notEmpty.wait_for(lock, timeout, [this] { return !m_buffer.empty() || m_closed; }))
			{
				return std::nullopt;
			}
			if (m_buffer.empty())
			{
				return std::nullopt;
			}
			T value = std::move(m_buffer.front());
			m_buffer.pop_front();
			m_notFull.notify_one();
			return value;
		}

		[[nodiscard]] std::optional<T> peek()
		{
			std::unique_lock lock(m_mutex);
			if (m_buffer.empty())
			{
				return std::nullopt;
			}
			return m_buffer.front();
		}

		// No coroutine waiters to resume on close: the former write_async/read_async
		// awaiter paths were deleted rather than fixed - nothing used them, and
		// close() resuming a waiter while holding m_mutex deadlocked any coroutine
		// that re-entered the channel. Blocking readers and writers wake from the
		// condition variables above.
		void close()
		{
			const std::scoped_lock l(m_mutex);
			m_closed = true;
			m_notEmpty.notify_all();
			m_notFull.notify_all();
		}

		[[nodiscard]] bool is_closed() const noexcept
		{
			std::scoped_lock l(m_mutex);
			return m_closed;
		}

		[[nodiscard]] std::size_t size() const noexcept
		{
			std::scoped_lock l(m_mutex);
			return m_buffer.size();
		}

		[[nodiscard]] std::size_t capacity() const noexcept
		{
			return m_capacity;
		}

	private:
		std::size_t m_capacity;
		std::deque<T> m_buffer;
		bool m_closed = false;

		mutable std::mutex m_mutex;
		std::condition_variable m_notEmpty;
		std::condition_variable m_notFull;

	};

} // namespace aether::coro
