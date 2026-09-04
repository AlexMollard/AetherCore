#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "net/NetTraversalSession.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/RoomCode.hpp"
#include "net/Signaling.hpp"

using namespace aether;
using State = net::TraversalState;

namespace
{
	// Mirrors the exact per-frame order NetworkContext::TickTraversal runs in -
	// Poll() first, so a punch response or the ENet handshake that follows an
	// opened path is visible to Tick() the same frame it arrived in.
	void PumpOnce(net::NetTraversalSession& session, net::NetworkSubsystem& transport, float dt)
	{
		transport.Poll();
		session.Tick(dt);
	}
} // namespace

TEST_CASE("Nothing is asked for before HostWithCode or JoinByCode runs")
{
	// Constructing one must not touch the router, a socket or a signalling
	// channel: most game processes never host or join at all, and a session that
	// does anything on construction shows up in someone's firewall log for no
	// reason.
	net::NetworkSubsystem transport;
	net::NetTraversalSession session(transport);

	CHECK(session.GetState() == State::Idle);
	CHECK(session.FailureReason().empty());
	CHECK_FALSE(session.JoinInProgress());
}

TEST_CASE("A malformed room code fails before anything is bound or published")
{
	net::NetworkSubsystem transport;
	net::NetTraversalSession session(transport);

	CHECK_FALSE(session.HostWithCode("", 24750, 4));
	CHECK(session.GetState() == State::Failed);
	CHECK_FALSE(session.FailureReason().empty());
	// Never even reached NetworkSubsystem::Host() - a bad code is refused before
	// the socket-level half of the ladder starts, the same as NatTraversal
	// refusing to punch at an empty candidate list rather than waiting it out.
	CHECK(transport.Traversal() == nullptr);
}

TEST_CASE("The ladder tries a router mapping first, and a failed one still reaches the punch")
{
	net::NetworkSubsystem transport;
	net::LocalSignalingChannel channel; // paired with nothing - this test never needs an answer
	net::NetTraversalSession session(transport);
	session.ConfigureSignalingChannel(channel);
	session.SetStunServer(""); // no real network in this test - see the class comment

	REQUIRE(session.HostWithCode(net::NewRoomCode(), 24751, 4));
	// PortMapping::Request() returns immediately (PortMappingTests.cpp exercises
	// this directly) - the FIRST rung of the ladder, before anything else runs.
	CHECK(session.GetState() == State::Mapping);

	// One oversized tick crosses NetTraversalSession's own give-up window for the
	// router's answer. This is deterministic regardless of whether a real router
	// exists wherever this runs: almost no real wall-clock time elapses in a
	// single call, so PortMapping's own background thread cannot plausibly have
	// resolved in that window either way - only NetTraversalSession's
	// SIMULATED-time budget, which this call blows straight past, decides the
	// outcome here.
	PumpOnce(session, transport, 6.0f);

	CHECK(session.GetState() != State::Mapping);
	CHECK((session.GetState() == State::Signaling || session.GetState() == State::Punching));
}

TEST_CASE("A peer that never shows up ends the ladder at Failed with a reason, not a loop")
{
	net::NetworkSubsystem transport;
	net::LocalSignalingChannel lonely; // paired with nothing, so nothing ever answers
	net::NetTraversalSession session(transport);
	session.ConfigureSignalingChannel(lonely);
	session.SetStunServer("");

	REQUIRE(session.HostWithCode(net::NewRoomCode(), 24752, 4));
	PumpOnce(session, transport, 6.0f); // past the mapping give-up window, as above
	REQUIRE(session.GetState() != State::Mapping);

	// NatRendezvous gives a peer twenty simulated seconds to say anything at all
	// before it calls the attempt off (see kPeerTimeout in NatRendezvous.cpp) -
	// which is exactly what standing in for a symmetric NAT looks like from this
	// session's side: no candidate this end offers is ever answered. The bound
	// below is generous so a loop that never terminates fails the test loudly
	// rather than hanging it.
	for (int i = 0; i < 1000 && session.GetState() != State::Failed; ++i)
	{
		PumpOnce(session, transport, 0.05f);
	}

	CHECK(session.GetState() == State::Failed);
	CHECK_FALSE(session.FailureReason().empty());
	CHECK_FALSE(session.JoinInProgress());
	// Torn down, not left claiming a socket a UI would have to close by hand -
	// see NetTraversalSession::Fail.
	CHECK(transport.Traversal() == nullptr);
}

TEST_CASE("A host and a joiner reach each other through the whole ladder and finish connected")
{
	// The whole point, end to end: neither side is told the other's address, the
	// room code is the only thing they share, and no port was ever forwarded.
	net::NetworkSubsystem hostTransport;
	net::NetworkSubsystem joinerTransport;
	net::LocalSignalingChannel hostChannel;
	net::LocalSignalingChannel joinerChannel;
	net::LocalSignalingChannel::Pair(hostChannel, joinerChannel);

	// Sessions are declared AFTER the channels and transports they hold
	// references to, so they are destroyed first - the ordinary RAII rule for a
	// reference member, and one this class leans on rather than re-checking.
	net::NetTraversalSession hostSession(hostTransport);
	net::NetTraversalSession joinerSession(joinerTransport);
	hostSession.ConfigureSignalingChannel(hostChannel);
	hostSession.SetStunServer("");
	joinerSession.ConfigureSignalingChannel(joinerChannel);
	joinerSession.SetStunServer("");

	const std::string code = net::NewRoomCode();
	REQUIRE(hostSession.HostWithCode(code, 24753, 4));
	REQUIRE(joinerSession.JoinByCode(code));

	CHECK(hostSession.GetState() == State::Mapping);
	// A joiner never asks the router - nothing is ever dialled INTO a joiner - so
	// it starts the ladder one rung further along than the host does.
	CHECK(joinerSession.GetState() == State::Signaling);
	CHECK(joinerSession.JoinInProgress());

	PumpOnce(hostSession, hostTransport, 6.0f); // clears the host's mapping window
	CHECK(hostSession.GetState() != State::Mapping);

	bool bothConnected = false;
	for (int i = 0; i < 800 && !bothConnected; ++i)
	{
		PumpOnce(hostSession, hostTransport, 0.05f);
		PumpOnce(joinerSession, joinerTransport, 0.05f);
		bothConnected = hostSession.GetState() == State::Connected && joinerSession.GetState() == State::Connected;
		// A small real wait, matching SignalingTests.cpp's own punched-connection
		// test: ENet's handshake is more than one datagram each way, and the
		// loopback interface still needs a little real time between service calls
		// to carry each leg of it.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(hostSession.GetState() == State::Connected);
	CHECK(joinerSession.GetState() == State::Connected);
	CHECK_FALSE(joinerSession.JoinInProgress());
	CHECK(hostSession.FailureReason().empty());
	CHECK(joinerSession.FailureReason().empty());
}
