#include <doctest/doctest.h>

#include "scripting/SceneContext.hpp"

using aether::app::scripting::ActiveContextScope;
using aether::app::scripting::g_activeContext;
using aether::app::scripting::SceneContext;

// Every path that calls into a script - OnAttach/OnUpdate/OnDetach and, since the RPC
// bridge, an inbound [NetRpc] dispatch - has to publish a scene context first, because
// the exports the script body calls dereference g_activeContext unconditionally. These
// pin the two rules that made the RPC path crash the host when it was missing.

TEST_CASE("A scope publishes its context and takes it back down")
{
	SceneContext ctx;
	REQUIRE(g_activeContext == nullptr);
	{
		const ActiveContextScope scope(ctx);
		CHECK(g_activeContext == &ctx);
	}
	CHECK(g_activeContext == nullptr);
}

TEST_CASE("A nested scope restores the outer context, not null")
{
	// The motivating case: a locally-routed RPC is dispatched from inside a script
	// update, so its scope nests inside the update's. Clearing to null on the way out
	// would leave the REST of that update running with no context - the same crash the
	// scope exists to prevent, just moved a few statements later.
	SceneContext outer;
	SceneContext inner;
	{
		const ActiveContextScope outerScope(outer);
		{
			const ActiveContextScope innerScope(inner);
			CHECK(g_activeContext == &inner);
		}
		CHECK(g_activeContext == &outer);
	}
	CHECK(g_activeContext == nullptr);
}
