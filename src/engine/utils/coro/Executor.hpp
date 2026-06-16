#pragma once

#include <coroutine>
#include <functional>
#include <mutex>
#include <vector>

namespace aether::coro
{

	// ---------------------------------------------------------------------------
	// Base executor: any component that can receive and schedule coroutine handles
	// for resumption.
	// ---------------------------------------------------------------------------
	class executor
	{
	public:
		virtual ~executor() = default;
		virtual void schedule(std::coroutine_handle<> h) = 0;
		[[nodiscard]] virtual const char* name() const noexcept = 0;

		// Drain all pending coroutines - returns how many were resumed.
		virtual std::size_t drain()
		{
			return 0;
		}

		// Convenience: schedule a coroutine via its owning executor (stored as
		// resume-context by schedule_on_resumer).
		static void schedule_on_resumer(std::coroutine_handle<> h);
	};

	// ---------------------------------------------------------------------------
	// Inline executor: resumes immediately on the scheduling thread.
	// Only safe when resumer and awaiter share the same thread / critical section.
	// ---------------------------------------------------------------------------
	class inline_executor final : public executor
	{
	public:
		void schedule(std::coroutine_handle<> h) override
		{
			h.resume();
		}

		[[nodiscard]] const char* name() const noexcept override
		{
			return "inline";
		}
	};

	// ---------------------------------------------------------------------------
	// Queued executor: accumulates coroutine handles then drains them on demand
	// (e.g. once per frame on the game thread).  Thread-safe.
	// ---------------------------------------------------------------------------
	class queued_executor final : public executor
	{
	public:
		void schedule(std::coroutine_handle<> h) override
		{
			std::scoped_lock l(m_mutex);
			m_pending.push_back(h);
		}

		std::size_t drain() override
		{
			std::vector<std::coroutine_handle<>> batch;
			{
				std::scoped_lock l(m_mutex);
				batch.swap(m_pending);
			}
			const auto n = batch.size();
			for (auto h: batch)
			{
				if (h)
				{
					h.resume();
				}
			}
			return n;
		}

		[[nodiscard]] const char* name() const noexcept override
		{
			return "queued";
		}

	private:
		std::mutex m_mutex;
		std::vector<std::coroutine_handle<>> m_pending;
	};

	// ---------------------------------------------------------------------------
	// Global default executor - set during application startup.
	// All cross-thread continuation resumptions go through this.
	// ---------------------------------------------------------------------------
	namespace detail
	{
		extern executor* g_default_executor;
	}

	inline void executor::schedule_on_resumer(std::coroutine_handle<> h)
	{
		if (auto e = detail::g_default_executor)
		{
			e->schedule(h);
		}
		else
		{
			// Fallback: resume inline (dangerous from I/O thread, but better than
			// dropping the continuation entirely).
			h.resume();
		}
	}

	inline void set_default_executor(executor* e)
	{
		detail::g_default_executor = e;
	}

	inline executor* get_default_executor()
	{
		return detail::g_default_executor;
	}

} // namespace aether::coro
