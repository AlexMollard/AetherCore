#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "net/PortMapping.hpp"

using namespace aether;
using State = net::PortMapping::State;

TEST_CASE("A mapping asks nothing of the router until it is told to")
{
	// Constructing one must not start a worker thread or send a discovery broadcast:
	// most sessions are single-player, and a game that shouts at the router on startup
	// is a game that shows up in someone's firewall log for no reason.
	net::PortMapping mapping;
	CHECK(mapping.GetState() == State::Idle);
	CHECK(mapping.ExternalPort() == 0);
	CHECK(mapping.ExternalHost().empty());

	// Releasing something never requested is a no-op, not a crash - teardown paths call
	// it unconditionally.
	mapping.Release();
	CHECK(mapping.GetState() == State::Idle);

	// Ticking before requesting must also do nothing; the frame loop calls it every
	// frame regardless of whether a session exists.
	mapping.Tick();
	CHECK(mapping.GetState() == State::Idle);
}

TEST_CASE("Requesting returns immediately rather than blocking on the network")
{
	// The contract that makes this usable from a frame loop: libplum spawns its own
	// worker and answers later, so Request must come straight back. If this ever starts
	// blocking, the game hitches for the router's timeout on the frame someone hosts.
	net::PortMapping mapping;

	const auto start = std::chrono::steady_clock::now();
	const bool started = mapping.Request(24719);
	const auto elapsed = std::chrono::steady_clock::now() - start;

	REQUIRE(started);
	CHECK(mapping.GetState() == State::Requesting);
	CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);

	mapping.Release();
	CHECK(mapping.GetState() == State::Idle);
}

TEST_CASE("Two mappings can exist at once without one's teardown breaking the other")
{
	// The library's own init is process-wide and NOT refcounted by it, so this is the
	// case that goes wrong: the first object to be destroyed calls the global cleanup
	// while the second is still using it.
	net::PortMapping first;
	net::PortMapping second;
	REQUIRE(first.Request(24720));
	REQUIRE(second.Request(24721));

	first.Release();
	CHECK(first.GetState() == State::Idle);

	// The second must still be live and answerable after the first let go.
	CHECK(second.GetState() == State::Requesting);
	second.Tick();
	CHECK(second.GetState() != State::Idle);

	second.Release();

	// And the library must come back up cleanly after the count reaches zero, which is
	// what a second session in one process does.
	net::PortMapping again;
	CHECK(again.Request(24722));
}

// Skipped by default: whether this succeeds depends on the router in the room, so it can
// only ever be run deliberately. It is the one test that proves the whole point - that a
// port opens without anyone touching a firewall - so run it on the machine you care about
// with --test-case="*real router*" --no-skip.
TEST_CASE("A real router opens a port when asked" * doctest::skip())
{
	net::PortMapping mapping;
	REQUIRE(mapping.Request(24723));

	// Long enough for the library's own default timeout to resolve one way or the other.
	for (int i = 0; i < 300 && mapping.GetState() == State::Requesting; ++i)
	{
		mapping.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	INFO("reason: " << mapping.FailureReason());
	// Either answer is a legitimate result here; what must never happen is being stuck
	// asking forever, because a player would sit watching a spinner that resolves never.
	REQUIRE(mapping.GetState() != State::Requesting);

	if (mapping.GetState() == State::Mapped)
	{
		MESSAGE("mapped external port: " << mapping.ExternalPort());
		MESSAGE("external host: " << mapping.ExternalHost());
		CHECK(mapping.ExternalPort() != 0);
	}
	else
	{
		MESSAGE("no mapping: " << mapping.FailureReason());
		CHECK_FALSE(mapping.FailureReason().empty());
	}
}
