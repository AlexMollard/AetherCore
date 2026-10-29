#include <coroutine>
#include <exception>
#include <thread>
#include <utility>

#include <doctest/doctest.h>
#include "utils/coro/Task.hpp"

namespace
{
	struct TrackedValue
	{
		int* destructions = nullptr;
		TrackedValue() = default;
		explicit TrackedValue(int& count) : destructions(&count) {}
		TrackedValue(const TrackedValue&) = delete;
		TrackedValue& operator=(const TrackedValue&) = delete;
		TrackedValue(TrackedValue&& other) noexcept : destructions(std::exchange(other.destructions, nullptr)) {}
		TrackedValue& operator=(TrackedValue&& other) noexcept
		{
			destructions = std::exchange(other.destructions, nullptr);
			return *this;
		}
		~TrackedValue()
		{
			if (destructions)
			{
				++*destructions;
			}
		}
	};

	struct ObservedCoroutine
	{
		struct promise_type
		{
			std::exception_ptr exception;
			ObservedCoroutine get_return_object() { return ObservedCoroutine{std::coroutine_handle<promise_type>::from_promise(*this)}; }
			std::suspend_always initial_suspend() noexcept { return {}; }
			std::suspend_always final_suspend() noexcept { return {}; }
			void return_void() noexcept {}
			void unhandled_exception() noexcept { exception = std::current_exception(); }
		};
		std::coroutine_handle<promise_type> handle;
		explicit ObservedCoroutine(std::coroutine_handle<promise_type> value) : handle(value) {}
		ObservedCoroutine(const ObservedCoroutine&) = delete;
		ObservedCoroutine& operator=(const ObservedCoroutine&) = delete;
		~ObservedCoroutine() { handle.destroy(); }
	};

	struct CompleteBetweenReadyAndSuspend
	{
		aether::coro::task<int>::awaiter awaiting;
		aether::coro::task_source<int> producer;
		bool await_ready()
		{
			const bool ready = awaiting.await_ready();
			std::thread worker([source = producer]() mutable { source.set_value(73); });
			worker.join();
			return ready;
		}
		bool await_suspend(std::coroutine_handle<> continuation)
		{
			return awaiting.await_suspend(continuation);
		}
		int await_resume() { return awaiting.await_resume(); }
	};

	ObservedCoroutine ObserveCompletion(CompleteBetweenReadyAndSuspend awaiting, int* result, int* resumptions)
	{
		*result = co_await awaiting;
		++*resumptions;
	}
}

TEST_CASE("Asynchronous task destroys an unconsumed result exactly once")
{
	int destructions = 0;
	{
		auto [task, producer] = aether::coro::task<TrackedValue>::create();
		producer.set_value(TrackedValue{destructions});
		REQUIRE(task.is_ready());
		CHECK(destructions == 0);
	}
	CHECK(destructions == 1);
}

TEST_CASE("Asynchronous completion between readiness and suspension resumes its waiter once")
{
	auto [task, producer] = aether::coro::task<int>::create();
	int result = -1;
	int resumptions = 0;
	auto observed = ObserveCompletion({std::move(task).operator co_await(), std::move(producer)}, &result, &resumptions);
	aether::coro::inline_executor immediate;
	auto* previous = aether::coro::get_default_executor();
	aether::coro::set_default_executor(&immediate);
	observed.handle.resume();
	aether::coro::set_default_executor(previous);
	if (observed.handle.promise().exception)
	{
		std::rethrow_exception(observed.handle.promise().exception);
	}
	CHECK(observed.handle.done());
	CHECK(result == 73);
	CHECK(resumptions == 1);
}
