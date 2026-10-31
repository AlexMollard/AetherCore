#include <doctest/doctest.h>

#include <stdexcept>

#include "rendering/ExclusiveMutation.hpp"
#include "rendering/RenderThreadState.hpp"

TEST_CASE("Exclusive mutation restores admission after mutation and idle failures")
{
	for (const bool failIdle: {false, true})
	{
		aether::RenderThreadState state;
		CHECK_THROWS_AS(aether::RunPausedMutation(state,
		        [&] { if (failIdle) { throw std::runtime_error("idle"); } },
		        [] { throw std::runtime_error("mutation"); }), std::runtime_error);
		CHECK_FALSE(state.IsReloadInProgress());
		CHECK(state.BeginFrame());
		state.CompleteFrame(0);
		bool called = false;
		aether::RunPausedMutation(state, [] {}, [&] { called = true; });
		CHECK(called);
		CHECK_FALSE(state.IsReloadInProgress());
	}
}
