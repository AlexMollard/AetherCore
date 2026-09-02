#include <doctest/doctest.h>

#include <string>

#include "net/NatRendezvous.hpp"
#include "net/NatTraversal.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/Signaling.hpp"

using namespace aether;
using Endpoint = net::NatTraversal::Endpoint;
using State = net::NatTraversal::State;

namespace
{
	Endpoint At(const char* address, std::uint16_t port)
	{
		const auto endpoint = net::NatTraversal::ParseEndpoint(address, port);
		REQUIRE(endpoint.has_value());
		return *endpoint;
	}
} // namespace

TEST_CASE("A candidate list survives the trip through a signalling channel")
{
	net::CandidateSet set;
	set.endpoints.push_back(At("192.168.1.40", 24710));
	set.endpoints.push_back(At("203.0.113.9", 41234));

	const std::string text = net::EncodeCandidates(set);
	const auto decoded = net::DecodeCandidates(text);
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->endpoints.size() == 2);
	CHECK(decoded->endpoints[0] == set.endpoints[0]);
	CHECK(decoded->endpoints[1] == set.endpoints[1]);

	// The port must survive exactly. It is the half of an endpoint a NAT rewrites, so a
	// truncation here aims every punch at the wrong hole while still looking plausible.
	CHECK(decoded->endpoints[1].port == 41234);
}

TEST_CASE("Anything not fully understood is refused rather than half-read")
{
	// A candidate list arrives from another machine. Salvaging part of a malformed one
	// means punching at an address nobody actually offered.
	CHECK_FALSE(net::DecodeCandidates("").has_value());
	CHECK_FALSE(net::DecodeCandidates("v1").has_value());                  // no candidates
	CHECK_FALSE(net::DecodeCandidates("v2 192.168.1.1:80").has_value());   // unknown version
	CHECK_FALSE(net::DecodeCandidates("192.168.1.1:80").has_value());      // no version
	CHECK_FALSE(net::DecodeCandidates("v1 192.168.1.1").has_value());      // no port
	CHECK_FALSE(net::DecodeCandidates("v1 192.168.1.1:").has_value());     // empty port
	CHECK_FALSE(net::DecodeCandidates("v1 :80").has_value());              // no address
	CHECK_FALSE(net::DecodeCandidates("v1 192.168.1.1:0").has_value());    // port zero
	CHECK_FALSE(net::DecodeCandidates("v1 192.168.1.1:65536").has_value()); // out of range
	CHECK_FALSE(net::DecodeCandidates("v1 192.168.1.1:80abc").has_value()); // trailing rubbish
	CHECK_FALSE(net::DecodeCandidates("v1 999.1.1.1:80").has_value());     // not an address
	CHECK_FALSE(net::DecodeCandidates("v1 example.invalid:80").has_value()); // needs a lookup

	// One good candidate beside one bad one still fails the whole set: accepting the
	// good half would let a peer smuggle a malformed entry in and be quietly forgiven.
	CHECK_FALSE(net::DecodeCandidates("v1 192.168.1.1:80 nonsense").has_value());
}

TEST_CASE("A peer cannot make this machine punch at an unbounded list of addresses")
{
	// The cap exists because candidates are remote input. Without it a peer names a
	// thousand strangers' addresses and this machine sends every one of them an
	// unsolicited datagram on its say-so.
	std::string text = "v1";
	for (std::size_t i = 0; i < net::kMaxCandidates; ++i)
	{
		text += " 192.168.1." + std::to_string(i + 1) + ":24710";
	}
	CHECK(net::DecodeCandidates(text).has_value());

	text += " 192.168.2.1:24710";
	CHECK_FALSE(net::DecodeCandidates(text).has_value());
}

TEST_CASE("A local channel carries an offer to the other end, not back to the sender")
{
	net::LocalSignalingChannel a;
	net::LocalSignalingChannel b;
	net::LocalSignalingChannel::Pair(a, b);

	net::CandidateSet set;
	set.endpoints.push_back(At("10.0.0.5", 24711));
	a.Publish(set);

	// Publishing must not deliver to the publisher: a peer that punched at its own
	// address would answer its own check and declare a path open to nowhere.
	CHECK_FALSE(a.Poll().has_value());

	const auto received = b.Poll();
	REQUIRE(received.has_value());
	REQUIRE(received->endpoints.size() == 1);
	CHECK(received->endpoints[0] == set.endpoints[0]);

	// Read once. A second poll with nothing new must say so rather than repeat, or the
	// rendezvous above restarts its punch window on every frame.
	CHECK_FALSE(b.Poll().has_value());
}

TEST_CASE("Two transports find each other knowing only a signalling channel")
{
	// The whole point, end to end: neither side is told the other's address, neither has
	// a forwarded port, and both are behind nothing but the channel between them.
	net::NetworkSubsystem a;
	net::NetworkSubsystem b;
	REQUIRE(a.Host(24712, 4));
	REQUIRE(b.Host(24713, 4));

	net::LocalSignalingChannel channelA;
	net::LocalSignalingChannel channelB;
	net::LocalSignalingChannel::Pair(channelA, channelB);

	// No STUN server: on a LAN there is no NAT to ask about, and the test must not
	// depend on a public host being reachable from wherever it runs.
	net::NatRendezvous rendezvousA(*a.Traversal(), channelA, 24712);
	net::NatRendezvous rendezvousB(*b.Traversal(), channelB, 24713);
	rendezvousA.Begin("");
	rendezvousB.Begin("");

	for (int i = 0; i < 200; ++i)
	{
		if (rendezvousA.GetState() == State::Open && rendezvousB.GetState() == State::Open)
		{
			break;
		}
		rendezvousA.Tick(0.05f);
		rendezvousB.Tick(0.05f);
		a.Poll();
		b.Poll();
	}

	CHECK(rendezvousA.GetState() == State::Open);
	CHECK(rendezvousB.GetState() == State::Open);

	// And the path names the peer, not just any endpoint that happened to answer.
	const auto path = rendezvousA.OpenPath();
	REQUIRE(path.has_value());
	CHECK(path->port == 24713);
}

TEST_CASE("A peer who never joins is reported as such, not as a NAT problem")
{
	net::NetworkSubsystem a;
	REQUIRE(a.Host(24714, 4));

	net::LocalSignalingChannel lonely; // paired with nothing, so nothing ever answers
	net::NatRendezvous rendezvous(*a.Traversal(), lonely, 24714);
	rendezvous.Begin("");

	for (int i = 0; i < 1000 && rendezvous.GetState() != State::Failed; ++i)
	{
		rendezvous.Tick(0.05f);
		a.Poll();
	}

	REQUIRE(rendezvous.GetState() == State::Failed);
	// The distinction is the point. Telling someone to reconfigure their router when
	// their friend simply has not pressed Join is an evening lost to the wrong problem.
	CHECK(rendezvous.FailureReason().find("joined") != std::string::npos);
}

TEST_CASE("An unreachable STUN server costs a candidate, not the connection")
{
	// Discovery failing means the public endpoint is unknown. It does not mean the peer
	// on this network has become unreachable, and treating it as fatal would strand two
	// players in one house because a server on the internet was down.
	net::NetworkSubsystem a;
	net::NetworkSubsystem b;
	REQUIRE(a.Host(24715, 4));
	REQUIRE(b.Host(24716, 4));

	net::LocalSignalingChannel channelA;
	net::LocalSignalingChannel channelB;
	net::LocalSignalingChannel::Pair(channelA, channelB);

	net::NatRendezvous rendezvousA(*a.Traversal(), channelA, 24715);
	net::NatRendezvous rendezvousB(*b.Traversal(), channelB, 24716);

	// Discard on loopback: resolvable, so discovery starts, and silent, so it times out.
	rendezvousA.Begin("127.0.0.1", 9);
	CHECK(rendezvousA.GetState() == State::Discovering);

	// A waits alone long enough for discovery to give up - past the five seconds it
	// allows the server, well short of the twenty it allows the peer.
	for (int i = 0; i < 160; ++i)
	{
		rendezvousA.Tick(0.05f);
		a.Poll();
	}
	// Underneath, discovery has failed. Above, the attempt is still alive - that is the
	// distinction, and reporting Failed here would have a caller abandon it.
	CHECK(a.Traversal()->GetState() == State::Failed);
	CHECK(rendezvousA.GetState() != State::Failed);

	rendezvousB.Begin("");

	for (int i = 0; i < 400; ++i)
	{
		if (rendezvousA.GetState() == State::Open)
		{
			break;
		}
		rendezvousA.Tick(0.05f);
		rendezvousB.Tick(0.05f);
		a.Poll();
		b.Poll();
	}

	CHECK(rendezvousA.GetState() == State::Open);
}
