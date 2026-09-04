#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "net/NatTraversal.hpp"
#include "net/RendezvousChannel.hpp"
#include "net/Signaling.hpp"

using namespace aether;
using Endpoint = net::NatTraversal::Endpoint;

namespace
{
	Endpoint At(const char* address, std::uint16_t port)
	{
		const auto endpoint = net::NatTraversal::ParseEndpoint(address, port);
		REQUIRE(endpoint.has_value());
		return *endpoint;
	}
} // namespace

TEST_CASE("A well-formed server datagram parses into its room code and candidates")
{
	// Hand-written, not built with EncodeRendezvousDatagram, so this exercises the
	// literal grammar the server speaks rather than only proving the encoder and
	// parser agree with each other.
	const std::string datagram = "AECR1 ABCDEF v1 192.168.1.40:24710 203.0.113.9:41234\n";

	const auto parsed = net::ParseRendezvousDatagram(datagram);
	REQUIRE(parsed.has_value());
	CHECK(parsed->roomCode == "ABCDEF");
	CHECK(parsed->blob == "v1 192.168.1.40:24710 203.0.113.9:41234");

	const auto interpreted = net::InterpretRendezvousDatagram(datagram, "ABCDEF");
	REQUIRE(interpreted.has_value());
	REQUIRE(interpreted->endpoints.size() == 2);
	CHECK(interpreted->endpoints[0] == At("192.168.1.40", 24710));
	CHECK(interpreted->endpoints[1] == At("203.0.113.9", 41234));
}

TEST_CASE("A candidate set round-trips through the channel's own serialiser")
{
	net::CandidateSet set;
	set.endpoints.push_back(At("10.0.0.5", 24712));
	set.endpoints.push_back(At("198.51.100.7", 55000));
	const std::string blob = net::EncodeCandidates(set);

	const std::string datagram = net::EncodeRendezvousDatagram("ROOM12", blob);
	CHECK(datagram == "AECR1 ROOM12 " + blob + "\n");

	const auto interpreted = net::InterpretRendezvousDatagram(datagram, "ROOM12");
	REQUIRE(interpreted.has_value());
	REQUIRE(interpreted->endpoints.size() == 2);
	CHECK(interpreted->endpoints[0] == set.endpoints[0]);
	CHECK(interpreted->endpoints[1] == set.endpoints[1]);

	// The server tolerates CRLF as well as a bare LF - a datagram terminated either
	// way must parse identically.
	std::string crlf = datagram;
	crlf.pop_back();
	crlf += "\r\n";
	const auto interpretedCrlf = net::InterpretRendezvousDatagram(crlf, "ROOM12");
	REQUIRE(interpretedCrlf.has_value());
	CHECK(interpretedCrlf->endpoints == interpreted->endpoints);
}

TEST_CASE("A datagram for a different room is ignored, not merely mismatched")
{
	net::CandidateSet set;
	set.endpoints.push_back(At("172.16.0.9", 24713));
	const std::string datagram = net::EncodeRendezvousDatagram("AAAAAA", net::EncodeCandidates(set));

	// The grammar itself is fine - this is a room filter, not a parse failure, and
	// the two must stay distinguishable so a caller can tell "not for me" from
	// "garbage" if it ever needs to.
	const auto parsed = net::ParseRendezvousDatagram(datagram);
	REQUIRE(parsed.has_value());
	CHECK(parsed->roomCode == "AAAAAA");

	CHECK_FALSE(net::InterpretRendezvousDatagram(datagram, "BBBBBB").has_value());
	CHECK(net::InterpretRendezvousDatagram(datagram, "AAAAAA").has_value());
}

TEST_CASE("A wrong or absent version prefix is refused")
{
	CHECK_FALSE(net::ParseRendezvousDatagram("AECX1 ABCDEF v1 1.2.3.4:80\n").has_value()); // wrong version
	CHECK_FALSE(net::ParseRendezvousDatagram("ABCDEF v1 1.2.3.4:80\n").has_value());        // no version at all
	CHECK_FALSE(net::ParseRendezvousDatagram("aecr1 ABCDEF v1 1.2.3.4:80\n").has_value());  // case is not tolerated
	CHECK_FALSE(net::ParseRendezvousDatagram("").has_value());
}

TEST_CASE("A truncated line is refused rather than read partially")
{
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR1 ABCDEF").has_value()); // no blob field at all
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR1").has_value());        // not even a room code
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR1 ").has_value());
}

TEST_CASE("An empty blob parses structurally but offers nothing to punch at")
{
	const std::string datagram = net::EncodeRendezvousDatagram("ABCDEF", "");

	// Structurally sound - three fields are present - so this is DecodeCandidates'
	// rejection to make, not ParseRendezvousDatagram's; the two stay separate so an
	// empty blob is diagnosed by the same code that already understands that grammar.
	const auto parsed = net::ParseRendezvousDatagram(datagram);
	REQUIRE(parsed.has_value());
	CHECK(parsed->roomCode == "ABCDEF");
	CHECK(parsed->blob.empty());

	CHECK_FALSE(net::InterpretRendezvousDatagram(datagram, "ABCDEF").has_value());
}

TEST_CASE("A blob offering more than kMaxCandidates endpoints is refused, not truncated")
{
	std::string blob = "v1";
	for (std::size_t i = 0; i < net::kMaxCandidates; ++i)
	{
		blob += " 192.168.1." + std::to_string(i + 1) + ":24710";
	}
	const std::string atCap = net::EncodeRendezvousDatagram("ABCDEF", blob);
	CHECK(net::InterpretRendezvousDatagram(atCap, "ABCDEF").has_value());

	blob += " 192.168.2.1:24710";
	const std::string overCap = net::EncodeRendezvousDatagram("ABCDEF", blob);
	CHECK_FALSE(net::InterpretRendezvousDatagram(overCap, "ABCDEF").has_value());
}

TEST_CASE("An over-long datagram is refused before its content is even inspected")
{
	// Structurally this would otherwise parse fine - version, room code, and a blob
	// field are all present - so this isolates the length cap itself rather than any
	// other rejection reason.
	const std::string longBlob(net::kMaxRendezvousDatagramLength, 'x');
	const std::string tooLong = "AECR1 ABCDEF " + longBlob + "\n";
	REQUIRE(tooLong.size() > net::kMaxRendezvousDatagramLength);
	CHECK_FALSE(net::ParseRendezvousDatagram(tooLong).has_value());

	// And the boundary itself must not be off by one: trim down to exactly the cap
	// and the same otherwise-malformed-content datagram parses structurally again.
	const std::string atBoundary = tooLong.substr(0, net::kMaxRendezvousDatagramLength);
	CHECK(net::ParseRendezvousDatagram(atBoundary).has_value());
}

TEST_CASE("Candidates accumulate across datagrams instead of the latest replacing the rest")
{
	// Mirrors why this matters end to end: a peer publishes its LAN candidates first
	// and its public endpoint only once STUN answers second, and discarding the
	// first would lose the address that works when both players share a network.
	std::vector<Endpoint> accumulated;

	net::CandidateSet firstDatagram;
	firstDatagram.endpoints.push_back(At("10.0.0.2", 24710));
	firstDatagram.endpoints.push_back(At("10.0.0.3", 24711));
	CHECK(net::AccumulateCandidates(accumulated, firstDatagram));
	REQUIRE(accumulated.size() == 2);

	net::CandidateSet secondDatagram;
	secondDatagram.endpoints.push_back(At("10.0.0.3", 24711));   // already known - must not duplicate
	secondDatagram.endpoints.push_back(At("203.0.113.4", 41234)); // new - the public endpoint, say
	CHECK(net::AccumulateCandidates(accumulated, secondDatagram));
	REQUIRE(accumulated.size() == 3);
	CHECK(accumulated[0] == firstDatagram.endpoints[0]);
	CHECK(accumulated[1] == firstDatagram.endpoints[1]);
	CHECK(accumulated[2] == secondDatagram.endpoints[1]);

	// A third datagram repeating only what is already known grows nothing.
	net::CandidateSet thirdDatagram;
	thirdDatagram.endpoints.push_back(At("10.0.0.3", 24711));
	CHECK_FALSE(net::AccumulateCandidates(accumulated, thirdDatagram));
	CHECK(accumulated.size() == 3);
}

TEST_CASE("Accumulation stops at kMaxCandidates instead of growing without bound")
{
	std::vector<Endpoint> accumulated;
	net::CandidateSet toCap;
	for (std::size_t i = 0; i < net::kMaxCandidates; ++i)
	{
		toCap.endpoints.push_back(At("192.168.1.1", static_cast<std::uint16_t>(24710 + i)));
	}
	CHECK(net::AccumulateCandidates(accumulated, toCap));
	REQUIRE(accumulated.size() == net::kMaxCandidates);

	net::CandidateSet oneMore;
	oneMore.endpoints.push_back(At("192.168.1.2", 30000)); // distinct from every one already held
	CHECK_FALSE(net::AccumulateCandidates(accumulated, oneMore));
	CHECK(accumulated.size() == net::kMaxCandidates);
}

TEST_CASE("A malformed rendezvous server address is a diagnosable failure, not a crash")
{
	// Every case here fails the "host:port" split itself, before any resolver or
	// socket call - deterministic, and requires no network of any kind.
	net::RendezvousChannel noAddress("", "ABCDEF");
	CHECK_FALSE(noAddress.IsUsable());
	CHECK_FALSE(noAddress.FailureReason().empty());

	net::RendezvousChannel noHost(":24701", "ABCDEF");
	CHECK_FALSE(noHost.IsUsable());

	net::RendezvousChannel badPortText("example.invalid:notaport", "ABCDEF");
	CHECK_FALSE(badPortText.IsUsable());

	net::RendezvousChannel zeroPort("example.invalid:0", "ABCDEF");
	CHECK_FALSE(zeroPort.IsUsable());

	net::RendezvousChannel hugePort("example.invalid:99999", "ABCDEF");
	CHECK_FALSE(hugePort.IsUsable());

	// Publish/Poll on a channel that never became usable are safe no-ops, not a
	// crash waiting for whoever forgot to check IsUsable() first.
	net::CandidateSet set;
	set.endpoints.push_back(At("10.0.0.5", 24710));
	noAddress.Publish(set);
	CHECK_FALSE(noAddress.Poll().has_value());
}

// The one test in this file that touches a real socket. Loopback always resolves and
// always binds, so this stays deterministic even though nothing answers - the same
// tolerance PortMappingTests.cpp gives a router that may not be in the room.
TEST_CASE("A channel with nothing listening still resolves, sends, and reports nothing")
{
	net::RendezvousChannel channel("127.0.0.1:24799", "ABCDEF");
	REQUIRE(channel.IsUsable());
	CHECK(channel.FailureReason().empty());

	net::CandidateSet set;
	set.endpoints.push_back(At("192.168.1.50", 24710));
	channel.Publish(set);

	for (int i = 0; i < 5; ++i)
	{
		CHECK_FALSE(channel.Poll().has_value());
	}
}
