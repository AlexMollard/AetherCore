#pragma once

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "Executor.hpp"

namespace aether::coro
{

	// ---------------------------------------------------------------------------
	// Bounded SPSC channel (single-producer, single-consumer) that supports both
	// blocking and coroutine-await operations.
	//
	// Internal locking makes it thread-safe for the general case; the "SPSC" in
	// the name refers to logical ownership (one writer, one reader), not a lock-
	// free guarantee.
	//
	// When the channel is full (write side) or empty (read side):
	//   - Blocking API  (write/wait_read) blocks the calling thread via condvar.
	//   - Coroutine API (write_async/read_async) suspends the calling coroutine
	//     and resumes it when space/data becomes available.
	// ---------------------------------------------------------------------------
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

		// -- Blocking API -------------------------------------------------------

		// Block until space is available, then write.
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

		// Block until data is available, then read.
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

		// Read with timeout.  Returns nullopt on timeout.
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

		// Peek: get the next value without removing it (blocks).
		[[nodiscard]] std::optional<T> peek()
		{
			std::unique_lock lock(m_mutex);
			if (m_buffer.empty())
			{
				return std::nullopt;
			}
			return m_buffer.front();
		}

		// -- Coroutine API ------------------------------------------------------

		// Awaitable for writing.
		class write_awaiter
		{
		public:
			write_awaiter(channel* ch, T value)
			      : m_channel(ch), m_value(std::move(value))
			{
			}

			bool await_ready() const noexcept
			{
				std::scoped_lock l(m_channel->m_mutex);
				return m_channel->m_buffer.size() < m_channel->m_capacity || m_channel->m_closed;
			}

			void await_suspend(std::coroutine_handle<> awaiting) noexcept
			{
				std::unique_lock lock(m_channel->m_mutex);
				if (m_channel->m_buffer.size() < m_channel->m_capacity || m_channel->m_closed)
				{
					lock.unlock();
					// Already have space - resume inline.
					if (!m_channel->m_closed)
					{
						std::scoped_lock l2(m_channel->m_mutex);
						m_channel->m_buffer.push_back(std::move(m_value));
						m_channel->m_notEmpty.notify_one();
					}
					awaiting.resume();
					return;
				}
				m_channel->m_writeWaiter = awaiting;
				m_channel->m_pendingWriteValue = std::move(m_value);
			}

			void await_resume() noexcept
			{
			}

		private:
			channel* m_channel;
			T m_value;
		};

		[[nodiscard]] write_awaiter write_async(T value)
		{
			return write_awaiter{this, std::move(value)};
		}

		// Awaitable for reading.
		class read_awaiter
		{
		public:
			explicit read_awaiter(channel* ch)
			      : m_channel(ch)
			{
			}

			bool await_ready() const noexcept
			{
				std::scoped_lock l(m_channel->m_mutex);
				return !m_channel->m_buffer.empty() || m_channel->m_closed;
			}

			void await_suspend(std::coroutine_handle<> awaiting) noexcept
			{
				std::unique_lock lock(m_channel->m_mutex);
				if (!m_channel->m_buffer.empty() || m_channel->m_closed)
				{
					lock.unlock();
					awaiting.resume();
					return;
				}
				m_channel->m_readWaiter = awaiting;
			}

			T await_resume()
			{
				std::scoped_lock lock(m_channel->m_mutex);
				if (m_channel->m_buffer.empty())
				{
					throw std::runtime_error("channel::read_async: closed with no data");
				}
				T value = std::move(m_channel->m_buffer.front());
				m_channel->m_buffer.pop_front();
				m_channel->m_notFull.notify_one();
				return value;
			}

		private:
			channel* m_channel;
		};

		[[nodiscard]] read_awaiter read_async()
		{
			return read_awaiter{this};
		}

		// -- Write-side notification (for the producer to notify the consumer) --
		// Called from the write side after producing data - resumes any awaiting
		// read coroutine directly.
		void notify_read_waiter()
		{
			std::coroutine_handle<> h = nullptr;
			{
				std::scoped_lock l(m_mutex);
				h = m_readWaiter;
				m_readWaiter = nullptr;
			}
			if (h)
			{
				h.resume();
			}
		}

		// -- Lifetime -----------------------------------------------------------

		void close()
		{
			std::scoped_lock l(m_mutex);
			m_closed = true;
			m_notEmpty.notify_all();
			m_notFull.notify_all();
			if (m_writeWaiter)
			{
				m_writeWaiter.resume();
				m_writeWaiter = nullptr;
			}
			if (m_readWaiter)
			{
				m_readWaiter.resume();
				m_readWaiter = nullptr;
			}
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

		// Single-slot coroutine waiters (SPSC: at most one waiter per direction)
		std::coroutine_handle<> m_writeWaiter = nullptr;
		T m_pendingWriteValue{};
		std::coroutine_handle<> m_readWaiter = nullptr;
	};

} // namespace aether::coro
