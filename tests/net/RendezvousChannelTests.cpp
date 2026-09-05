#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <enet/enet.h>

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

	// A raw loopback socket standing in for whichever rendezvous server the
	// channel was pointed at - and, for the spoof half of one test, for a stranger
	// who is NOT that server.
	class RawPeer
	{
	public:
		explicit RawPeer(std::uint16_t port)
		      : m_port(port)
		{
			m_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
			REQUIRE(m_socket != ENET_SOCKET_NULL);
			ENetAddress bind{};
			bind.host = ENET_HOST_ANY;
			bind.port = port;
			REQUIRE(enet_socket_bind(m_socket, &bind) == 0);
			REQUIRE(enet_socket_set_option(m_socket, ENET_SOCKOPT_NONBLOCK, 1) == 0);
		}

		~RawPeer()
		{
			enet_socket_destroy(m_socket);
		}

		RawPeer(const RawPeer&) = delete;
		RawPeer& operator=(const RawPeer&) = delete;

		// Waits (bounded) for one datagram, returning its text and where it came
		// from - the channel's ephemeral port is learned this way, exactly the way
		// a real server learns it.
		std::optional<std::pair<std::string, ENetAddress>> ReceiveFrom()
		{
			for (int i = 0; i < 50; ++i)
			{
				std::array<char, 1500> buffer{};
				ENetBuffer wire{};
				wire.data = buffer.data();
				wire.dataLength = buffer.size();
				ENetAddress from{};
				const int received = enet_socket_receive(m_socket, &from, &wire, 1);
				if (received > 0)
				{
					return std::make_pair(std::string(buffer.data(), static_cast<std::size_t>(received)), from);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return std::nullopt;
		}

		void ReplyTo(const ENetAddress& to, std::string_view payload)
		{
			ENetBuffer wire{};
			wire.data = const_cast<char*>(payload.data());
			wire.dataLength = payload.size();
			enet_socket_send(m_socket, &to, &wire, 1);
		}

	private:
		ENetSocket m_socket = ENET_SOCKET_NULL;
		std::uint16_t m_port = 0;
	};
} // namespace

TEST_CASE("A well-formed server datagram parses into room, token, and body")
{
	// Hand-written, not built with EncodeRendezvousDatagram, so this exercises the
	// literal grammar the server speaks rather than only proving the encoder and
	// parser agree with each other.
	const std::string datagram = "AECR2 ABCDEF 0123456789ABCDEF v1 192.168.1.40:24710 203.0.113.9:41234\n";

	const auto parsed = net::ParseRendezvousDatagram(datagram);
	REQUIRE(parsed.has_value());
	CHECK(parsed->roomCode == "ABCDEF");
	CHECK(parsed->token == "0123456789ABCDEF");
	CHECK(parsed->body == "v1 192.168.1.40:24710 203.0.113.9:41234");

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

	const std::string datagram = net::EncodeRendezvousDatagram("ROOM12", "-", blob);
	CHECK(datagram == "AECR2 ROOM12 - " + blob + "\n");

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

	// Lower-case hex is accepted even though the server emits upper-case: a token
	// that survives the round trip in one case must not be dropped for the other.
	const auto lower = net::ParseRendezvousDatagram("AECR2 ROOM12 0123456789abcdef " + blob + "\n");
	REQUIRE(lower.has_value());
	CHECK(lower->token == "0123456789abcdef");
}

TEST_CASE("A handshake-only datagram carries a token and deliberately no body")
{
	// The server's answer to a token-less first contact. It parses, the token is
	// readable, but the body is "-" and must not be decoded as candidates.
	const std::string datagram = net::EncodeRendezvousDatagram("ABCDEF", "0123456789ABCDEF", "-");
	CHECK(datagram == "AECR2 ABCDEF 0123456789ABCDEF -\n");

	const auto parsed = net::ParseRendezvousDatagram(datagram);
	REQUIRE(parsed.has_value());
	CHECK(parsed->token == "0123456789ABCDEF");
	CHECK(parsed->body == "-");
	CHECK_FALSE(net::InterpretRendezvousDatagram(datagram, "ABCDEF").has_value());
}

TEST_CASE("A token that is not dash or sixteen hex characters is refused")
{
	const std::string blob = "v1 10.0.0.5:24710";
	CHECK_FALSE(net::ParseRendezvousDatagram(net::EncodeRendezvousDatagram("ABCDEF", "12345", blob)).has_value());            // too short
	CHECK_FALSE(net::ParseRendezvousDatagram(net::EncodeRendezvousDatagram("ABCDEF", "1123456789ABCDEF0", blob)).has_value());  // too long
	CHECK_FALSE(net::ParseRendezvousDatagram(net::EncodeRendezvousDatagram("ABCDEF", "zzzzzzzzzzzzzzzz", blob)).has_value());   // not hex
	CHECK_FALSE(net::ParseRendezvousDatagram(net::EncodeRendezvousDatagram("ABCDEF", "", blob)).has_value());                   // empty is not dash
}

TEST_CASE("A datagram for a different room is ignored, not merely mismatched")
{
	net::CandidateSet set;
	set.endpoints.push_back(At("172.16.0.9", 24713));
	const std::string datagram = net::EncodeRendezvousDatagram("AAAAAA", "0123456789ABCDEF", net::EncodeCandidates(set));

	// The grammar itself is fine - this is a room filter, not a parse failure, and
	// the two must stay distinguishable so a caller can tell "not for me" from
	// "garbage" if it ever needs to.
	const auto parsed = net::ParseRendezvousDatagram(datagram);
	REQUIRE(parsed.has_value());
	CHECK(parsed->roomCode == "AAAAAA");

	CHECK_FALSE(net::InterpretRendezvousDatagram(datagram, "BBBBBB").has_value());
	CHECK(net::InterpretRendezvousDatagram(datagram, "AAAAAA").has_value());
}

TEST_CASE("A wrong or absent version prefix is refused - AECR1 included")
{
	CHECK_FALSE(net::ParseRendezvousDatagram("AECX2 ABCDEF - v1 1.2.3.4:80\n").has_value()); // wrong version
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR1 ABCDEF v1 1.2.3.4:80\n").has_value());   // the dead ancestor: no token field
	CHECK_FALSE(net::ParseRendezvousDatagram("ABCDEF - v1 1.2.3.4:80\n").has_value());       // no version at all
	CHECK_FALSE(net::ParseRendezvousDatagram("aecr2 ABCDEF - v1 1.2.3.4:80\n").has_value()); // case is not tolerated
	CHECK_FALSE(net::ParseRendezvousDatagram("").has_value());
}

TEST_CASE("A truncated line is refused rather than read partially")
{
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR2 ABCDEF").has_value());         // no token field at all
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR2").has_value());                // not even a room code
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR2 ").has_value());
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR2 ABCDEF 0123456789ABCDEF").has_value());  // token with no body separator
	CHECK_FALSE(net::ParseRendezvousDatagram("AECR2 ABCDEF 0123456789ABCDEF ").has_value()); // separator with no body after it
}

TEST_CASE("A body that fails the candidate grammar is refused, not salvaged")
{
	// Three fields are present and the token is fine, so the body is
	// DecodeCandidates' rejection to make - the layers stay separate so an empty
	// body is diagnosed by the code that already understands that grammar.
	CHECK_FALSE(net::InterpretRendezvousDatagram("AECR2 ABCDEF 0123456789ABCDEF v1\n", "ABCDEF").has_value());
	CHECK_FALSE(net::InterpretRendezvousDatagram("AECR2 ABCDEF 0123456789ABCDEF nonsense\n", "ABCDEF").has_value());
}

TEST_CASE("A blob offering more than kMaxCandidates endpoints is refused, not truncated")
{
	std::string blob = "v1";
	for (std::size_t i = 0; i < net::kMaxCandidates; ++i)
	{
		blob += " 192.168.1." + std::to_string(i + 1) + ":24710";
	}
	const std::string atCap = net::EncodeRendezvousDatagram("ABCDEF", "0123456789ABCDEF", blob);
	CHECK(net::InterpretRendezvousDatagram(atCap, "ABCDEF").has_value());

	blob += " 192.168.2.1:24710";
	const std::string overCap = net::EncodeRendezvousDatagram("ABCDEF", "0123456789ABCDEF", blob);
	CHECK_FALSE(net::InterpretRendezvousDatagram(overCap, "ABCDEF").has_value());
}

TEST_CASE("An over-long datagram is refused before its content is even inspected")
{
	// Structurally this would otherwise parse fine - version, room code, token and
	// a body field are all present - so this isolates the length cap itself rather
	// than any other rejection reason.
	const std::string longBlob(net::kMaxRendezvousDatagramLength, 'x');
	const std::string tooLong = "AECR2 ABCDEF 0123456789ABCDEF " + longBlob + "\n";
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

// The tests below touch real loopback sockets. Loopback always resolves and
// always binds, so they stay deterministic even though only scripts and test
// peers answer - the same tolerance PortMappingTests.cpp gives a router that
// may not be in the room.
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

TEST_CASE("First contact carries no token; the token is learned from the server's answer and used immediately")
{
	constexpr std::uint16_t kServerPort = 24794;
	RawPeer server(kServerPort);
	net::RendezvousChannel channel("127.0.0.1:" + std::to_string(kServerPort), "ABCDEF");
	REQUIRE(channel.IsUsable());

	net::CandidateSet ours;
	ours.endpoints.push_back(At("10.0.0.5", 24710));
	channel.Publish(ours);

	// The first datagram the server sees is a token-less first contact.
	const auto first = server.ReceiveFrom();
	REQUIRE(first.has_value());
	CHECK(first->first == "AECR2 ABCDEF - v1 10.0.0.5:24710\n");

	// The server answers handshake-only: here is your token, nothing else.
	const std::string token = "0123456789ABCDEF";
	server.ReplyTo(first->second, net::EncodeRendezvousDatagram("ABCDEF", token, "-"));
	CHECK_FALSE(channel.Poll().has_value()); // a token is not candidates

	// Poll stored the token and re-sent the outstanding blob with it straight
	// away, rather than waiting out the two-second retransmit timer.
	const auto resend = server.ReceiveFrom();
	REQUIRE(resend.has_value());
	CHECK(resend->first == "AECR2 ABCDEF " + token + " v1 10.0.0.5:24710\n");
}

TEST_CASE("A re-issued token replaces the stale one and is presented from then on")
{
	// The server may forget a peer and mint it a fresh token - its tables evict
	// under flood, and it restarts. Recovery is the client's job: a peer that
	// keeps presenting the token it was issued first has its blob ignored from
	// then on, so the far peer dials nothing and the join dies with both sides
	// believing they published. Adopting whatever token the latest accepted line
	// carries is what makes eviction survivable.
	constexpr std::uint16_t kServerPort = 24793;
	RawPeer server(kServerPort);
	net::RendezvousChannel channel("127.0.0.1:" + std::to_string(kServerPort), "ABCDEF");
	REQUIRE(channel.IsUsable());

	net::CandidateSet ours;
	ours.endpoints.push_back(At("10.0.0.5", 24710));
	channel.Publish(ours);

	const auto first = server.ReceiveFrom();
	REQUIRE(first.has_value());

	const std::string stale = "0123456789ABCDEF";
	server.ReplyTo(first->second, net::EncodeRendezvousDatagram("ABCDEF", stale, "-"));
	CHECK_FALSE(channel.Poll().has_value());

	const auto withStale = server.ReceiveFrom();
	REQUIRE(withStale.has_value());
	REQUIRE(withStale->first == "AECR2 ABCDEF " + stale + " v1 10.0.0.5:24710\n");

	// Evicted: the server answers that blob with a handshake-only line carrying a
	// DIFFERENT token.
	const std::string fresh = "FEDCBA9876543210";
	server.ReplyTo(withStale->second, net::EncodeRendezvousDatagram("ABCDEF", fresh, "-"));
	CHECK_FALSE(channel.Poll().has_value());

	// Adopted immediately, on the re-send the token change itself triggers.
	const auto resend = server.ReceiveFrom();
	REQUIRE(resend.has_value());
	CHECK(resend->first == "AECR2 ABCDEF " + fresh + " v1 10.0.0.5:24710\n");

	// And it sticks: a later publish carries the fresh token, not the stale one.
	net::CandidateSet more;
	more.endpoints.push_back(At("10.0.0.6", 24711));
	channel.Publish(more);
	const auto later = server.ReceiveFrom();
	REQUIRE(later.has_value());
	CHECK(later->first.find(" " + fresh + " ") != std::string::npos);
	CHECK(later->first.find(stale) == std::string::npos);
}

TEST_CASE("Only the configured server's datagrams are accepted - anyone else's are dropped")
{
	// The channel's socket is bound to ANY:ephemeral and the room code is public,
	// so a datagram whose grammar and room code match still proves nothing about
	// who sent it. One spoofed line must neither inject candidates nor freeze the
	// channel's own retransmission (sawReply) - the real server never hears this
	// peer again and the join dies quietly.
	constexpr std::uint16_t kServerPort = 24795;

	RawPeer server(kServerPort);
	net::RendezvousChannel channel("127.0.0.1:" + std::to_string(kServerPort), "ABCDEF");
	REQUIRE(channel.IsUsable());

	net::CandidateSet ours;
	ours.endpoints.push_back(At("10.0.0.5", 24710));
	channel.Publish(ours);

	// The publish tells the fake server the channel's ephemeral port, the same way
	// the real one learns it.
	const auto channelAddress = server.ReceiveFrom();
	REQUIRE(channelAddress.has_value());

	const std::string token = "0123456789ABCDEF";
	net::CandidateSet theirs;
	theirs.endpoints.push_back(At("192.168.1.40", 24710));
	const std::string reply = net::EncodeRendezvousDatagram("ABCDEF", token, net::EncodeCandidates(theirs));

	// From the server: accepted.
	server.ReplyTo(channelAddress->second, reply);
	const auto accepted = channel.Poll();
	REQUIRE(accepted.has_value());
	REQUIRE(accepted->endpoints.size() == 1);
	CHECK(accepted->endpoints[0] == theirs.endpoints[0]);

	// The same bytes from a DIFFERENT source: dropped before anything is parsed.
	// Bound to its own ephemeral port so its source address cannot be the server's.
	RawPeer stranger(0);
	net::CandidateSet injected;
	injected.endpoints.push_back(At("203.0.113.66", 6666));
	stranger.ReplyTo(channelAddress->second, net::EncodeRendezvousDatagram("ABCDEF", token, net::EncodeCandidates(injected)));
	CHECK_FALSE(channel.Poll().has_value());

	// And the stranger's line did not blind retransmission either: the channel is
	// still willing to hear the server again, proven by another accepted reply.
	net::CandidateSet more;
	more.endpoints.push_back(At("192.168.1.41", 24711));
	server.ReplyTo(channelAddress->second, net::EncodeRendezvousDatagram("ABCDEF", token, net::EncodeCandidates(more)));
	const auto second = channel.Poll();
	REQUIRE(second.has_value());
	CHECK(std::ranges::find(second->endpoints, injected.endpoints[0]) == second->endpoints.end());
}
