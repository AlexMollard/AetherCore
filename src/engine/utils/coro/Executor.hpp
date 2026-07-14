#pragma once

#include <coroutine>
#include <functional>
#include <mutex>
#include <vector>

namespace aether::coro
{

	class executor
	{
	public:
		virtual ~executor() = default;
		virtual void schedule(std::coroutine_handle<> h) = 0;
		[[nodiscard]] virtual const char* name() const noexcept = 0;

		virtual std::size_t drain()
		{
			return 0;
		}

		static void schedule_on_resumer(std::coroutine_handle<> h);
	};

	// Inline executor: resumes immediately on the scheduling thread.
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

	// (e.g. once per frame on the game thread).  Thread-safe.
	class queued_executor final : public executor
	{
	public:
		void schedule(std::coroutine_handle<> h) override
		{
			const std::scoped_lock l(m_mutex);
			m_pending.push_back(h);
		}

		std::size_t drain() override
		{
			std::vector<std::coroutine_handle<>> batch;
			{
				const std::scoped_lock l(m_mutex);
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

	// All cross-thread continuation resumptions go through this.
	namespace detail
	{
		extern executor* g_default_executor;
	}

	inline void executor::schedule_on_resumer(std::coroutine_handle<> h)
	{
		if (auto* e = detail::g_default_executor)
		{
			e->schedule(h);
		}
		else
		{
			// Fallback: resume inline (dangerous from I/O thread, but better than
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
