#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "net/BroadcastSignaling.hpp"
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

TEST_CASE("A configured rendezvous seeds the backend, but never overrides what the game asked for")
{
	// The settings cascade is re-read on EVERY HostWithCode/JoinByCode, which is
	// AFTER a game has had its chance to choose - so a default that overrode would
	// silently undo the game's choice on the second join of a session, and only
	// over the internet, where it is hardest to notice.
	SUBCASE("nothing configured stays on the rung that needs no server")
	{
		net::NetworkSubsystem transport;
		net::NetTraversalSession session(transport);
		session.SetRendezvousDefault("", 24701); // the empty default in EngineSettings
		CHECK(session.Backend() == net::SignalingBackend::LanBroadcast);
		CHECK(session.SignalingAddress().empty());
	}

	SUBCASE("a configured host is adopted when the game chose nothing")
	{
		net::NetworkSubsystem transport;
		net::NetTraversalSession session(transport);
		session.SetRendezvousDefault("rendezvous.example.com", 24701);
		CHECK(session.Backend() == net::SignalingBackend::Rendezvous);
		CHECK(session.SignalingAddress() == "rendezvous.example.com:24701");
	}

	SUBCASE("the game's own choice wins, however often settings are re-read")
	{
		net::NetworkSubsystem transport;
		net::NetTraversalSession session(transport);
		session.ConfigureSignaling(net::SignalingBackend::Rendezvous, "chosen.example.com:9000");
		session.SetRendezvousDefault("configured.example.com", 24701);
		session.SetRendezvousDefault("configured.example.com", 24701); // as a second join would
		CHECK(session.SignalingAddress() == "chosen.example.com:9000");
	}

	SUBCASE("a game that deliberately chose LAN is not dragged onto a rendezvous")
	{
		net::NetworkSubsystem transport;
		net::NetTraversalSession session(transport);
		session.ConfigureSignaling(net::SignalingBackend::LanBroadcast, "");
		session.SetRendezvousDefault("configured.example.com", 24701);
		CHECK(session.Backend() == net::SignalingBackend::LanBroadcast);
	}
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

TEST_CASE("A host and a joiner reach each other over a REAL broadcast socket, not a mocked channel" * doctest::skip())
{
	// The test above proves the ladder's STATE MACHINE is correct when signaling
	// is a direct in-memory pairing (LocalSignalingChannel) - instant, lossless,
	// same process. It does not touch a single real socket. This test closes
	// that gap: two REAL BroadcastSignalingChannel instances, on a dedicated
	// test port so a live production session on the real default (24700) is
	// never collided with, carrying the SAME room code over an actual OS UDP
	// broadcast the way two real Editor instances on the same LAN do. Skipped
	// by default for the same reason BroadcastSignalingTests.cpp's own real-
	// socket test is: whether a broadcast actually leaves the interface depends
	// on firewall/network configuration on the machine running it - run with
	// --test-case="*REAL broadcast socket*" --no-skip.
	constexpr std::uint16_t kTestBroadcastPort = 24798;

	net::NetworkSubsystem hostTransport;
	net::NetworkSubsystem joinerTransport;
	const std::string code = net::NewRoomCode();
	net::BroadcastSignalingChannel hostChannel(code, kTestBroadcastPort);
	net::BroadcastSignalingChannel joinerChannel(code, kTestBroadcastPort);
	REQUIRE(hostChannel.IsUsable());
	REQUIRE(joinerChannel.IsUsable());

	net::NetTraversalSession hostSession(hostTransport);
	net::NetTraversalSession joinerSession(joinerTransport);
	hostSession.ConfigureSignalingChannel(hostChannel);
	hostSession.SetStunServer("");
	joinerSession.ConfigureSignalingChannel(joinerChannel);
	joinerSession.SetStunServer("");

	REQUIRE(hostSession.HostWithCode(code, 24788, 4));
	REQUIRE(joinerSession.JoinByCode(code));

	PumpOnce(hostSession, hostTransport, 6.0f); // clears the host's mapping window
	REQUIRE(hostSession.GetState() != State::Mapping);

	bool bothConnected = false;
	for (int i = 0; i < 800 && !bothConnected; ++i)
	{
		PumpOnce(hostSession, hostTransport, 0.05f);
		PumpOnce(joinerSession, joinerTransport, 0.05f);
		bothConnected = hostSession.GetState() == State::Connected && joinerSession.GetState() == State::Connected;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	// If this fails while the mocked-channel test above passes, the defect is
	// specific to the REAL broadcast path (wire encoding, OS delivery, or the
	// receive-side room/nonce filter in NetTraversalSession::ParseLine) rather
	// than to the traversal ladder's own state machine.
	INFO("host state=" << (int)hostSession.GetState() << " reason=" << hostSession.FailureReason());
	INFO("joiner state=" << (int)joinerSession.GetState() << " reason=" << joinerSession.FailureReason());
	CHECK(hostSession.GetState() == State::Connected);
	CHECK(joinerSession.GetState() == State::Connected);
	CHECK_FALSE(joinerSession.JoinInProgress());
}

TEST_CASE("Relaying is skipped with no relay configured at all, and the ladder ends at Failed exactly as before")
{
	net::NetworkSubsystem transport;
	net::LocalSignalingChannel lonely; // paired with nothing, so nothing ever answers
	net::NetTraversalSession session(transport);
	session.ConfigureSignalingChannel(lonely);
	session.SetStunServer("");
	// Neither SetTurnServer call at all - the class's own defaults (empty turnHost,
	// allowRelay false) mirror EngineSettings::Network's, and must behave identically.

	REQUIRE(session.HostWithCode(net::NewRoomCode(), 24780, 4));
	PumpOnce(session, transport, 6.0f); // past the mapping give-up window
	REQUIRE(session.GetState() != State::Mapping);

	for (int i = 0; i < 1000 && session.GetState() != State::Failed; ++i)
	{
		REQUIRE(session.GetState() != State::Relaying);
		PumpOnce(session, transport, 0.05f);
	}

	CHECK(session.GetState() == State::Failed);
	CHECK_FALSE(session.FailureReason().empty());
}

TEST_CASE("Relaying is skipped when a relay host is set but allowRelay is left off")
{
	net::NetworkSubsystem transport;
	net::LocalSignalingChannel lonely;
	net::NetTraversalSession session(transport);
	session.ConfigureSignalingChannel(lonely);
	session.SetStunServer("");
	// A real host, but allowRelay stays false: relaying is never tried just because a
	// server is NAMED - see NatTraversal.hpp's class comment on why that stays opt-in.
	session.SetTurnServer("127.0.0.1", 24781, "user", "pass", false);

	REQUIRE(session.HostWithCode(net::NewRoomCode(), 24782, 4));
	PumpOnce(session, transport, 6.0f);
	REQUIRE(session.GetState() != State::Mapping);

	for (int i = 0; i < 1000 && session.GetState() != State::Failed; ++i)
	{
		REQUIRE(session.GetState() != State::Relaying);
		PumpOnce(session, transport, 0.05f);
	}

	CHECK(session.GetState() == State::Failed);
	CHECK_FALSE(session.FailureReason().empty());
}

TEST_CASE("With a relay configured, a failed punch advances to Relaying rather than Failed")
{
	net::NetworkSubsystem transport;
	net::LocalSignalingChannel lonely; // paired with nothing, so the punch genuinely fails
	net::NetTraversalSession session(transport);
	session.ConfigureSignalingChannel(lonely);
	session.SetStunServer("");
	// A syntactically valid TURN endpoint is all BeginRelay needs to start - nothing
	// here has to actually answer for THIS assertion, only for the next test.
	session.SetTurnServer("127.0.0.1", 24783, "user", "pass", true);

	REQUIRE(session.HostWithCode(net::NewRoomCode(), 24784, 4));
	PumpOnce(session, transport, 6.0f);
	REQUIRE(session.GetState() != State::Mapping);

	for (int i = 0; i < 1000 && session.GetState() != State::Relaying && session.GetState() != State::Failed; ++i)
	{
		PumpOnce(session, transport, 0.05f);
	}

	CHECK(session.GetState() == State::Relaying);
	CHECK(session.FailureReason().empty()); // not failed - the ladder is still running
	CHECK(transport.Traversal() != nullptr); // the socket the relay needs is still up
}

TEST_CASE("A relay that never answers ends the ladder at Failed, naming the relay - distinct from the no-relay reason")
{
	net::NetworkSubsystem transport;
	net::LocalSignalingChannel lonely;
	net::NetTraversalSession session(transport);
	session.ConfigureSignalingChannel(lonely);
	session.SetStunServer("");
	// Nothing is listening on this port - standing in for a relay that never answers,
	// the same way NatTraversalTests.cpp's own "Punching at nowhere" test uses a
	// discard-style port to stand in for a peer that never replies.
	session.SetTurnServer("127.0.0.1", 24785, "user", "pass", true);

	REQUIRE(session.HostWithCode(net::NewRoomCode(), 24786, 4));
	PumpOnce(session, transport, 6.0f);
	REQUIRE(session.GetState() != State::Mapping);

	for (int i = 0; i < 1000 && session.GetState() != State::Relaying; ++i)
	{
		PumpOnce(session, transport, 0.05f);
	}
	REQUIRE(session.GetState() == State::Relaying);

	// TurnClient's own Allocate give-up needs real backoff time to elapse (RFC 5389
	// s7.2.1 exponential retransmit, up to kMaxSendsPerTransaction attempts) - a run of
	// oversized ticks, the same trick this file already uses for PortMapping's window,
	// crosses it in a bounded number of calls rather than thousands of small ones.
	for (int i = 0; i < 20 && session.GetState() != State::Failed; ++i)
	{
		PumpOnce(session, transport, 100.0f);
	}

	CHECK(session.GetState() == State::Failed);
	const std::string& reason = session.FailureReason();
	CHECK_FALSE(reason.empty());
	CHECK(reason.find("relay") != std::string::npos);
	// Distinct from the plain punch-failure wording (NatTraversal.cpp's own "no path to
	// the peer opened... likely symmetric" message) this ladder used to end on with no
	// relay configured - a relay that is reachable but silent is a different problem.
	CHECK(reason.find("symmetric") == std::string::npos);
}

TEST_CASE("A peer that connects while the ladder is still running outranks the ladder")
{
	// The Mapped rung keeps the ladder running after publishing the mapping's
	// endpoint (a mapping is a candidate, not a verdict), which makes this race
	// real: a joiner can complete ENet's handshake through the mapping while this
	// side's own punch at its candidates is still failing - or, as staged here,
	// while the signalling channel offers nothing at all. Without the event watch
	// in TickRendezvous the peer's own twenty-second timeout then Fails the
	// attempt and Disconnects a connection that is up and carrying traffic.
	net::NetworkSubsystem hostTransport;
	net::LocalSignalingChannel lonely; // paired with nothing: no candidate is ever offered
	net::NetTraversalSession hostSession(hostTransport);
	hostSession.ConfigureSignalingChannel(lonely);
	hostSession.SetStunServer("");

	REQUIRE(hostSession.HostWithCode(net::NewRoomCode(), 24787, 4));
	PumpOnce(hostSession, hostTransport, 6.0f); // past the mapping give-up window
	REQUIRE(hostSession.GetState() == State::Signaling);

	// A direct-IP joiner - standing in for one that arrived through a mapping this
	// session published and cannot see from inside the ladder.
	net::NetworkSubsystem joinerTransport;
	REQUIRE(joinerTransport.Connect("127.0.0.1", 24787));

	bool connected = false;
	for (int i = 0; i < 500 && !connected; ++i)
	{
		PumpOnce(hostSession, hostTransport, 0.05f);
		joinerTransport.Poll();
		// Past the joiner's whole ENet handshake on the first iterations; past the
		// host's twenty-second peer timeout by the end, so a session that ignored
		// the connection fails the loop instead of passing it by attrition.
		connected = hostSession.GetState() == State::Connected && joinerTransport.Role() == net::NetRole::Client;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	CHECK(connected);
	// The live socket was not torn down around the session's own rung bookkeeping.
	CHECK(hostTransport.Traversal() != nullptr);
}
