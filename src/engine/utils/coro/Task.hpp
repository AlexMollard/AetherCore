#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include "Executor.hpp"

namespace aether::coro
{

	template<typename T>
	class async
	{
	public:
		struct promise_type
		{
			T m_value{};
			std::exception_ptr m_exception;
			std::coroutine_handle<> m_caller;

			auto initial_suspend() noexcept
			{
				return std::suspend_always{};
			}

			auto final_suspend() noexcept
			{
				struct final_awaiter
				{
					bool await_ready() noexcept
					{
						return false;
					}

					std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
					{
						if (h.promise().m_caller)
						{
							return h.promise().m_caller;
						}
						return std::noop_coroutine();
					}

					void await_resume() noexcept
					{
					}
				};

				return final_awaiter{};
			}

			void unhandled_exception() noexcept
			{
				m_exception = std::current_exception();
			}

			template<typename U>
			void return_value(U&& v) noexcept
			{
				m_value = std::forward<U>(v);
			}

			async get_return_object() noexcept
			{
				return async{std::coroutine_handle<promise_type>::from_promise(*this)};
			}
		};

		async() noexcept = default;

		explicit async(std::coroutine_handle<promise_type> h) noexcept
		      : m_handle(h)
		{
		}

		async(const async&) = delete;
		async& operator=(const async&) = delete;

		async(async&& other) noexcept
		      : m_handle(std::exchange(other.m_handle, nullptr))
		{
		}

		async& operator=(async&& other) noexcept
		{
			if (this != &other)
			{
				if (m_handle)
				{
					m_handle.destroy();
				}
				m_handle = std::exchange(other.m_handle, nullptr);
			}
			return *this;
		}

		~async()
		{
			if (m_handle)
			{
				m_handle.destroy();
			}
		}

		class awaiter
		{
		public:
			explicit awaiter(std::coroutine_handle<promise_type> h) noexcept
			      : m_handle(h)
			{
			}

			[[nodiscard]] bool await_ready() const noexcept
			{
				return false;
			}

			std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
			{
				m_handle.promise().m_caller = caller;
				return m_handle;
			}

			T await_resume()
			{
				if (m_handle.promise().m_exception)
				{
					std::rethrow_exception(m_handle.promise().m_exception);
				}
				return std::move(m_handle.promise().m_value);
			}

		private:
			std::coroutine_handle<promise_type> m_handle;
		};

		[[nodiscard]] awaiter operator co_await() &
		{
			return awaiter{m_handle};
		}

		[[nodiscard]] awaiter operator co_await() &&
		{
			return awaiter{m_handle};
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_handle != nullptr;
		}

	private:
		std::coroutine_handle<promise_type> m_handle{nullptr};
	};

	// a different thread.  The task is created via task<T>::create(), which

	template<typename T>
	class task;

	template<typename T>
	class task_source;

	namespace detail
	{

		template<typename T>
		struct task_state
		{
			std::atomic<bool> ready{false};
			std::exception_ptr error;
			T value;
			std::coroutine_handle<> continuation;

			task_state() = default;
			task_state(const task_state&) = delete;
			task_state& operator=(const task_state&) = delete;
			task_state(task_state&&) = delete;
			task_state& operator=(task_state&&) = delete;

			~task_state()
			{
				if (ready.load(std::memory_order_acquire) && !error)
				{
					value.~T();
				}
			}
		};

		template<>
		struct task_state<void>
		{
			std::atomic<bool> ready{false};
			std::exception_ptr error;
			std::coroutine_handle<> continuation;

			task_state() = default;
			~task_state() = default;
			task_state(const task_state&) = delete;
			task_state& operator=(const task_state&) = delete;
			task_state(task_state&&) = delete;
			task_state& operator=(task_state&&) = delete;
		};

	} // namespace detail

	template<typename T>
	class task_source
	{
	public:
		using state_type = detail::task_state<T>;

		explicit task_source(std::shared_ptr<state_type> state) noexcept
		      : m_state(std::move(state))
		{
		}

		void set_value(T v) noexcept
		{
			auto st = m_state.get();
			if constexpr (!std::is_void_v<T>)
			{
				st->value = std::move(v);
			}
			st->ready.store(true, std::memory_order_release);
			auto c = std::exchange(st->continuation, nullptr);
			if (c)
			{
				executor::schedule_on_resumer(c);
			}
		}

		void set_exception(const std::exception_ptr& e) noexcept
		{
			auto st = m_state.get();
			st->error = std::move(e);
			st->ready.store(true, std::memory_order_release);
			auto c = std::exchange(st->continuation, nullptr);
			if (c)
			{
				executor::schedule_on_resumer(c);
			}
		}

		[[nodiscard]] bool is_ready() const noexcept
		{
			return m_state && m_state->ready.load(std::memory_order_acquire);
		}

	private:
		std::shared_ptr<state_type> m_state;
	};

	template<typename T>
	class task
	{
	public:
		using state_type = detail::task_state<T>;
		using source_type = task_source<T>;

		task() noexcept = default;

		explicit task(std::shared_ptr<state_type> state) noexcept
		      : m_state(std::move(state))
		{
		}

		task(const task&) = delete;
		task& operator=(const task&) = delete;

		task(task&&) noexcept = default;
		task& operator=(task&&) noexcept = default;
		~task() = default;

		class awaiter
		{
		public:
			explicit awaiter(std::shared_ptr<state_type> state) noexcept
			      : m_state(std::move(state))
			{
			}

			[[nodiscard]] bool await_ready() const noexcept
			{
				return m_state && m_state->ready.load(std::memory_order_acquire);
			}

			bool await_suspend(std::coroutine_handle<> awaiting) noexcept
			{
				auto st = m_state.get();
				st->continuation = awaiting;

				if (st->ready.load(std::memory_order_acquire))
				{
					st->continuation = nullptr;
					executor::schedule_on_resumer(awaiting);
					return false;
				}
				return true;
			}

			decltype(auto) await_resume()
			{
				if (!m_state)
				{
					throw std::runtime_error("task::awaiter: no shared state");
				}
				auto st = m_state.get();
				if (st->error)
				{
					std::rethrow_exception(st->error);
				}
				if constexpr (!std::is_void_v<T>)
				{
					return std::move(st->value);
				}
			}

		private:
			std::shared_ptr<state_type> m_state;
		};

		[[nodiscard]] awaiter operator co_await() &
		{
			return awaiter{m_state};
		}

		[[nodiscard]] awaiter operator co_await() &&
		{
			return awaiter{std::move(m_state)};
		}

		[[nodiscard]] bool is_ready() const noexcept
		{
			return m_state && m_state->ready.load(std::memory_order_acquire);
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_state != nullptr;
		}

		T wait()
		{
			if (!m_state)
			{
				throw std::runtime_error("task::wait: empty task");
			}
			auto st = m_state.get();
			while (!st->ready.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			if (st->error)
			{
				std::rethrow_exception(st->error);
			}
			if constexpr (!std::is_void_v<T>)
			{
				return std::move(st->value);
			}
		}

		[[nodiscard]] static task make_ready(T v)
		{
			auto state = std::make_shared<state_type>();
			if constexpr (!std::is_void_v<T>)
			{
				state->value = std::move(v);
			}
			state->ready.store(true, std::memory_order_release);
			return task{std::move(state)};
		}

		[[nodiscard]] static task make_exception(std::exception_ptr e)
		{
			auto state = std::make_shared<state_type>();
			state->error = std::move(e);
			state->ready.store(true, std::memory_order_release);
			return task{std::move(state)};
		}

		[[nodiscard]] static std::pair<task, source_type> create()
		{
			auto state = std::make_shared<state_type>();
			task t{state};
			source_type s{state};
			return {std::move(t), std::move(s)};
		}

	private:
		std::shared_ptr<state_type> m_state;
	};

} // namespace aether::coro
