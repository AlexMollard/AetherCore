#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <enet/enet.h>

#include "net/BroadcastSignaling.hpp"
#include "net/NatTraversal.hpp"

using namespace aether;

namespace
{
	// A room code and two nonces already in the canonical (normalized) shape, used
	// throughout so each test only has to vary the one thing it is checking.
	constexpr const char* kRoom = "ABCDEF";
	constexpr const char* kOwnNonce = "1111111111111111";
	constexpr const char* kPeerNonce = "2222222222222222";

	std::string ValidBlob()
	{
		net::CandidateSet set;
		const auto endpoint = net::NatTraversal::ParseEndpoint("192.168.1.40", 24710);
		REQUIRE(endpoint.has_value());
		set.endpoints.push_back(*endpoint);
		return net::EncodeCandidates(set);
	}

	std::string Line(std::string_view room, std::string_view nonce, std::string_view blob)
	{
		return std::string("AECB1 ") + std::string(room) + " " + std::string(nonce) + " " + std::string(blob) + "\n";
	}
} // namespace

TEST_CASE("A well-formed line from another peer decodes to its candidates")
{
	const std::string line = Line(kRoom, kPeerNonce, ValidBlob());
	const auto result = net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce);
	REQUIRE(result.has_value());
	REQUIRE(result->endpoints.size() == 1);
	CHECK(result->endpoints[0].port == 24710);
}

TEST_CASE("A line missing its trailing newline is still accepted")
{
	std::string line = Line(kRoom, kPeerNonce, ValidBlob());
	line.pop_back(); // drop the '\n' - a raw datagram payload need not carry one
	CHECK(net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce).has_value());
}

TEST_CASE("A datagram carrying this channel's own nonce is ignored")
{
	// Broadcast reaches the sender too - without this check a channel would hear
	// its own offer and try to punch at itself.
	const std::string line = Line(kRoom, kOwnNonce, ValidBlob());
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce).has_value());
}

TEST_CASE("A datagram for a different room is ignored")
{
	const std::string line = Line("GHJKMN", kPeerNonce, ValidBlob());
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce).has_value());
}

TEST_CASE("Truncated and malformed lines are refused, not partially read")
{
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine("", kRoom, kOwnNonce).has_value());
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine("AECB1", kRoom, kOwnNonce).has_value());
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(std::string("AECB1 ") + kRoom, kRoom, kOwnNonce).has_value());
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(std::string("AECB1 ") + kRoom + " " + kPeerNonce, kRoom, kOwnNonce).has_value()); // no blob at all
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(std::string("AECB1 ") + kRoom + " " + kPeerNonce + " ", kRoom, kOwnNonce).has_value()); // trailing space, still no blob
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(Line(kRoom, kPeerNonce, ValidBlob()).substr(0, 3), kRoom, kOwnNonce).has_value()); // cut mid-tag
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine("XXXX1 " + std::string(kRoom) + " " + kPeerNonce + " " + ValidBlob(), kRoom, kOwnNonce).has_value()); // wrong tag entirely
}

TEST_CASE("A missing nonce field is refused rather than misread from the blob")
{
	// One field short: what would be the nonce is actually the start of the
	// candidate blob ("v1"), and two characters must not pass as sixteen hex
	// digits by accident.
	const std::string line = std::string("AECB1 ") + kRoom + " " + ValidBlob() + "\n";
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce).has_value());
}

TEST_CASE("A nonce that is not sixteen hex characters is refused")
{
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(Line(kRoom, "12345", ValidBlob()), kRoom, kOwnNonce).has_value());           // too short
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(Line(kRoom, "11111111111111111", ValidBlob()), kRoom, kOwnNonce).has_value()); // too long
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(Line(kRoom, "zzzzzzzzzzzzzzzz", ValidBlob()), kRoom, kOwnNonce).has_value());  // not hex
}

TEST_CASE("An over-long line is refused before it is parsed at all")
{
	std::string line = std::string("AECB1 ") + kRoom + " " + kPeerNonce + " v1";
	while (line.size() <= net::BroadcastSignalingChannel::kMaxLineLength)
	{
		line += " 192.168.1.1:24710";
	}
	CHECK(line.size() > net::BroadcastSignalingChannel::kMaxLineLength);
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce).has_value());
}

TEST_CASE("More candidates than the cap allows is refused")
{
	std::string blob = "v1";
	for (std::size_t i = 0; i < net::kMaxCandidates + 1; ++i)
	{
		blob += " 192.168.1." + std::to_string(i + 1) + ":24710";
	}
	const std::string line = Line(kRoom, kPeerNonce, blob);
	// Confirms the candidate cap, not the line-length cap, is what rejects this -
	// the two are different defences and this test must exercise the right one.
	REQUIRE(line.size() <= net::BroadcastSignalingChannel::kMaxLineLength);
	CHECK_FALSE(net::BroadcastSignalingChannel::ParseLine(line, kRoom, kOwnNonce).has_value());
}

TEST_CASE("An unusable channel reports why instead of throwing or crashing")
{
	// An empty string is not six characters of anything, room code or otherwise -
	// construction must fail cleanly rather than assert or misbehave on it.
	net::BroadcastSignalingChannel channel("", 24700);
	CHECK_FALSE(channel.IsUsable());
	CHECK_FALSE(channel.FailureReason().empty());

	// And every operation on an unusable channel is a safe no-op, not a crash on a
	// socket that was never opened.
	const auto endpoint = net::NatTraversal::ParseEndpoint("10.0.0.1", 1234);
	REQUIRE(endpoint.has_value());
	net::CandidateSet set;
	set.endpoints.push_back(*endpoint);
	channel.Publish(set);
	CHECK_FALSE(channel.Poll().has_value());
}

namespace
{
	// One raw loopback socket standing in for a hostile LAN peer: it can put
	// anything on this machine's receive queue, and only the channel's own drain
	// bound decides how much of the frame one Poll() spends on it.
	class RawSender
	{
	public:
		RawSender()
		{
			m_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
			REQUIRE(m_socket != ENET_SOCKET_NULL);
			ENetAddress any{};
			any.host = ENET_HOST_ANY;
			any.port = ENET_PORT_ANY;
			REQUIRE(enet_socket_bind(m_socket, &any) == 0);
			REQUIRE(enet_socket_set_option(m_socket, ENET_SOCKOPT_NONBLOCK, 1) == 0);
		}

		~RawSender()
		{
			enet_socket_destroy(m_socket);
		}

		RawSender(const RawSender&) = delete;
		RawSender& operator=(const RawSender&) = delete;

		void SendTo(std::uint16_t port, std::string_view payload)
		{
			ENetAddress to{};
			to.host = ENET_HOST_TO_NET_32(0x7F000001); // 127.0.0.1
			to.port = port;
			ENetBuffer buffer{};
			buffer.data = const_cast<char*>(payload.data());
			buffer.dataLength = payload.size();
			enet_socket_send(m_socket, &to, &buffer, 1);
		}

	private:
		ENetSocket m_socket = ENET_SOCKET_NULL;
	};
} // namespace

TEST_CASE("A flooded socket bounds one Poll - what is left waits for the next call")
{
	// Pre-fix, Poll() drained until the socket was empty; on a real LAN a peer can
	// keep it non-empty forever, which is a frame-loop hang, not a slow poll. What
	// a bounded flood can still prove is WHERE the drain stops: the valid line
	// queued behind more datagrams than one Poll reads must come back on the NEXT
	// Poll, not this one.
	constexpr std::uint16_t kPort = 24797; // away from the game default and every other net test
	net::BroadcastSignalingChannel channel(kRoom, kPort);
	REQUIRE(channel.IsUsable());
	CHECK(channel.FailureReason().empty()); // a usable channel carries no failure text

	RawSender flooder;
	for (int i = 0; i < 70; ++i)
	{
		flooder.SendTo(kPort, "garbage that fails the parse"); // recv happens before the parse
	}
	const auto endpoint = net::NatTraversal::ParseEndpoint("192.168.1.40", 24710);
	REQUIRE(endpoint.has_value());
	flooder.SendTo(kPort, Line(kRoom, kPeerNonce, ValidBlob())); // 71st: valid, and LAST

	// Loopback delivers same-pair datagrams in order, so all 70 garbage lines sit
	// ahead of the valid one. A Poll bounded at 64 stops before reaching it.
	CHECK_FALSE(channel.Poll().has_value());

	const auto received = channel.Poll();
	REQUIRE(received.has_value());
	REQUIRE(received->endpoints.size() == 1);
	CHECK(received->endpoints[0] == *endpoint);
}

// Skipped by default: whether a broadcast actually leaves the interface depends on
// the firewall and network configuration of the machine running it, so - like the
// real-router test in PortMappingTests.cpp - this can only be run deliberately.
// Run with --test-case="*real broadcast socket*" --no-skip.
TEST_CASE("Two channels in one process hear each other over a real broadcast socket" * doctest::skip())
{
	constexpr std::uint16_t kTestPort = 24799; // away from the game's default, so a stray real session cannot collide
	net::BroadcastSignalingChannel a("TESTAB", kTestPort);
	net::BroadcastSignalingChannel b("TESTAB", kTestPort);

	if (!a.IsUsable() || !b.IsUsable())
	{
		MESSAGE("broadcast socket unavailable: " << (!a.IsUsable() ? a.FailureReason() : b.FailureReason()));
		return; // a blocked broadcast is a legitimate result here, not a test failure
	}

	const auto endpoint = net::NatTraversal::ParseEndpoint("192.168.1.50", 24710);
	REQUIRE(endpoint.has_value());
	net::CandidateSet set;
	set.endpoints.push_back(*endpoint);

	std::optional<net::CandidateSet> received;
	for (int i = 0; i < 50 && !received.has_value(); ++i)
	{
		a.Publish(set);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		received = b.Poll();
	}

	if (!received.has_value())
	{
		MESSAGE("no datagram arrived - broadcast may be blocked or filtered on this machine");
		return;
	}

	REQUIRE(received->endpoints.size() == 1);
	CHECK(received->endpoints[0] == set.endpoints[0]);
}
