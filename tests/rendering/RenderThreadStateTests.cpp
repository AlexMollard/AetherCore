#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <thread>

#include "rendering/RenderThreadState.hpp"

namespace
{
	using namespace std::chrono_literals;

	// Rescue a failing waiter before joining, so the regression reports a failure
	// instead of leaving the entire unit suite blocked indefinitely.
	bool FinishWait(aether::RenderThreadState& state, std::future<void>& finished)
	{
		const bool ready = finished.wait_for(2s) == std::future_status::ready;
		if (!ready)
		{
			state.Shutdown();
		}
		finished.get();
		return ready;
	}
}

TEST_CASE("Render completion retains progress before a waiter starts")
{
	aether::RenderThreadState state;
	REQUIRE(state.BeginFrame());
	state.CompleteFrame(4);
	auto finished = std::async(std::launch::async, [&] { state.WaitUntilFrameCompleted(4); });
	CHECK(FinishWait(state, finished));
}

TEST_CASE("Render completion cannot be satisfied by an earlier frame")
{
	aether::RenderThreadState state;
	auto finished = std::async(std::launch::async, [&] { state.WaitUntilFrameCompleted(5); });
	state.CompleteFrame(4);
	CHECK(finished.wait_for(20ms) == std::future_status::timeout);
	state.CompleteFrame(5);
	CHECK(FinishWait(state, finished));
}

TEST_CASE("A final render completion wakes racing waiters without a later frame")
{
	for (int iteration = 0; iteration < 128; ++iteration)
	{
		aether::RenderThreadState state;
		REQUIRE(state.BeginFrame());
		std::promise<void> started;
		auto ready = started.get_future();
		auto finished = std::async(std::launch::async,
		        [&]
		        {
			        started.set_value();
			        state.WaitUntilFrameCompleted(0);
		        });
		ready.wait();
		state.CompleteFrame(0);
		CHECK(FinishWait(state, finished));
	}
}

TEST_CASE("Reload waits for an admitted frame and blocks new frame admission")
{
	aether::RenderThreadState state;
	REQUIRE(state.BeginFrame());
	state.SetReloadInProgress(true);
	auto paused = std::async(std::launch::async, [&] { state.WaitPaused(); });
	CHECK(paused.wait_for(20ms) == std::future_status::timeout);
	state.CompleteFrame(0);
	CHECK(FinishWait(state, paused));
	CHECK_FALSE(state.BeginFrame());
	state.SetReloadInProgress(false);
	CHECK(state.BeginFrame());
	state.CompleteFrame(1);
}

TEST_CASE("Reload resume and shutdown wake waiters racing their final transition")
{
	for (int iteration = 0; iteration < 64; ++iteration)
	{
		aether::RenderThreadState state;
		state.SetReloadInProgress(true);
		auto resumed = std::async(std::launch::async, [&] { state.WaitForReload(); });
		state.SetReloadInProgress(false);
		CHECK(FinishWait(state, resumed));

		REQUIRE(state.BeginFrame());
		state.SetReloadInProgress(true);
		auto completed = std::async(std::launch::async, [&] { state.WaitUntilFrameCompleted(0); });
		auto paused = std::async(std::launch::async, [&] { state.WaitPaused(); });
		auto reload = std::async(std::launch::async, [&] { state.WaitForReload(); });
		state.Shutdown();
		CHECK(FinishWait(state, completed));
		CHECK(FinishWait(state, paused));
		CHECK(FinishWait(state, reload));
		CHECK_FALSE(state.BeginFrame());
	}
}
