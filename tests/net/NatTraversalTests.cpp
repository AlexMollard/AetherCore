#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "net/NatTraversal.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/Signaling.hpp"

using namespace aether;
using Endpoint = net::NatTraversal::Endpoint;
using State = net::NatTraversal::State;

namespace
{
	// Runs both sides forward together. A punch only completes when each has sent toward
	// the other, so stepping one to completion before touching the other would never open
	// anything - the same reason it does not work to try candidates strictly in turn.
	void PumpBoth(net::NetworkSubsystem& a, net::NetworkSubsystem& b, int iterations = 200)
	{
		for (int i = 0; i < iterations; ++i)
		{
			if (a.Traversal()->GetState() == State::Open && b.Traversal()->GetState() == State::Open)
			{
				return;
			}
			a.Traversal()->Tick(0.05f);
			b.Traversal()->Tick(0.05f);
			a.Poll();
			b.Poll();
		}
	}
} // namespace

TEST_CASE("An address survives being written down and read back")
{
	// Candidates cross a signalling channel as text, and the two halves of ENet's
	// address convention disagree - host in network order, port in host order - so a
	// round trip is where a byte-order slip would surface.
	const auto endpoint = net::NatTraversal::ParseEndpoint("127.0.0.1", 24701);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->port == 24701);
	CHECK(net::NatTraversal::FormatAddress(*endpoint) == "127.0.0.1");

	const auto routable = net::NatTraversal::ParseEndpoint("203.0.113.7", 9);
	REQUIRE(routable.has_value());
	CHECK(net::NatTraversal::FormatAddress(*routable) == "203.0.113.7");

	// A host name is refused rather than resolved: a candidate is an address a NAT
	// already reported, so anything needing a lookup did not come from one.
	CHECK_FALSE(net::NatTraversal::ParseEndpoint("example.invalid", 80).has_value());
}

TEST_CASE("Traversal exists for a socket the transport owns, and dies with it")
{
	net::NetworkSubsystem host;
	CHECK(host.Traversal() == nullptr); // nothing to punch from yet

	REQUIRE(host.Host(24702, 4));
	REQUIRE(host.Traversal() != nullptr);
	CHECK(host.Traversal()->GetState() == State::Idle);

	// Tearing the transport down must take the intercept with it; a callback outliving
	// its owner fires on the next datagram that happens to arrive.
	host.Disconnect();
	CHECK(host.Traversal() == nullptr);
}

TEST_CASE("Two peers punch a path to each other over their own transport sockets")
{
	net::NetworkSubsystem a;
	net::NetworkSubsystem b;
	REQUIRE(a.Host(24703, 4));
	REQUIRE(b.Host(24704, 4));

	const auto toB = net::NatTraversal::ParseEndpoint("127.0.0.1", 24704);
	const auto toA = net::NatTraversal::ParseEndpoint("127.0.0.1", 24703);
	REQUIRE(toB.has_value());
	REQUIRE(toA.has_value());

	const std::vector<Endpoint> bCandidates{*toB};
	const std::vector<Endpoint> aCandidates{*toA};
	a.Traversal()->BeginPunch(bCandidates);
	b.Traversal()->BeginPunch(aCandidates);
	CHECK(a.Traversal()->GetState() == State::Punching);

	PumpBoth(a, b);

	CHECK(a.Traversal()->GetState() == State::Open);
	CHECK(b.Traversal()->GetState() == State::Open);

	// The endpoint that answered is what a connect must then be aimed at: the mapping
	// belongs to that exact pair, and any other address has no hole to travel through.
	const auto opened = a.Traversal()->OpenPath();
	REQUIRE(opened.has_value());
	CHECK(opened->port == 24704);
}

TEST_CASE("Punching at nowhere fails with a reason a player can act on")
{
	net::NetworkSubsystem a;
	REQUIRE(a.Host(24705, 4));

	// Discard, which answers nothing - standing in for the peer whose NAT gives every
	// destination a different mapping, so no check it sends can ever be answered.
	const auto nowhere = net::NatTraversal::ParseEndpoint("127.0.0.1", 9);
	REQUIRE(nowhere.has_value());
	const std::vector<Endpoint> candidates{*nowhere};
	a.Traversal()->BeginPunch(candidates);

	for (int i = 0; i < 300 && a.Traversal()->GetState() == State::Punching; ++i)
	{
		a.Traversal()->Tick(0.05f);
		a.Poll();
	}

	CHECK(a.Traversal()->GetState() == State::Failed);
	// Naming the cause matters: "connection failed" sends someone to their router for a
	// setting that cannot help, which is the wrong half of the day to lose.
	CHECK(a.Traversal()->FailureReason().find("relay") != std::string::npos);
}

TEST_CASE("Being asked to punch at nothing is refused rather than waited out")
{
	net::NetworkSubsystem a;
	REQUIRE(a.Host(24706, 4));

	a.Traversal()->BeginPunch({});
	CHECK(a.Traversal()->GetState() == State::Failed);
}

TEST_CASE("BeginPunch arms no more checks than the candidate cap, whatever it is handed")
{
	// BeginPunch turns a candidate into unsolicited datagrams on a 250ms timer, so
	// the cap the wire side enforces is enforced here too - a caller handing in an
	// over-long span must not be able to arm a spray wider than any peer could ever
	// legitimately offer. Nothing is ticked, so nothing is sent.
	net::NetworkSubsystem a;
	REQUIRE(a.Host(24708, 4));

	std::vector<Endpoint> flood;
	for (int i = 0; i < 32; ++i)
	{
		const auto endpoint = net::NatTraversal::ParseEndpoint("127.0.0.1", static_cast<std::uint16_t>(21000 + i));
		REQUIRE(endpoint.has_value());
		flood.push_back(*endpoint);
	}

	a.Traversal()->BeginPunch(flood);
	CHECK(a.Traversal()->GetState() == State::Punching);
	CHECK(a.Traversal()->PeerCandidates().size() == net::kMaxCandidates);
}

// Skipped by default: it needs the internet, and a test suite that fails when a
// third party's server is down is a test suite people learn to ignore. Run it on
// demand with --test-case="*real STUN*" --no-skip after touching StunMessage or the
// discovery path - everything else here proves the messages are well-formed by our own
// reading of RFC 5389, which is exactly the assumption a real server can falsify.
TEST_CASE("Discovery works against a real STUN server" * doctest::skip())
{
	net::NetworkSubsystem host;
	REQUIRE(host.Host(24707, 4));
	REQUIRE(host.Traversal()->BeginDiscovery("stun.l.google.com", 19302));

	// Real seconds, because a real round trip takes them.
	for (int i = 0; i < 250 && host.Traversal()->GetState() == State::Discovering; ++i)
	{
		host.Traversal()->Tick(0.02f);
		host.Poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	INFO("failure reason: " << host.Traversal()->FailureReason());
	REQUIRE(host.Traversal()->GetState() == State::Discovered);

	const auto reflexive = host.Traversal()->PublicEndpoint();
	REQUIRE(reflexive.has_value());
	MESSAGE("public endpoint: " << net::NatTraversal::FormatAddress(*reflexive) << ':' << reflexive->port);

	// A parse that silently produced zeroes would look like success otherwise.
	CHECK(reflexive->host != 0);
	CHECK(reflexive->port != 0);
}

TEST_CASE("Local candidates name real interfaces, without repeating one")
{
	const auto candidates = net::NatTraversal::LocalCandidates(24709);
	REQUIRE_FALSE(candidates.empty());

	for (const Endpoint& candidate: candidates)
	{
		// The port must be the one the transport is bound to, not one the probe picked:
		// a peer punching at an ephemeral probe port would reach nothing.
		CHECK(candidate.port == 24709);
		CHECK(candidate.host != 0);
		MESSAGE("local candidate: " << net::NatTraversal::FormatAddress(candidate));
	}

	// Duplicates cost a wasted connectivity check on every retry and eat into the cap a
	// peer's candidate list is allowed, for no chance of opening anything new.
	for (std::size_t i = 0; i < candidates.size(); ++i)
	{
		for (std::size_t j = i + 1; j < candidates.size(); ++j)
		{
			CHECK_FALSE(candidates[i] == candidates[j]);
		}
	}
}

namespace
{
	// RFC 5389 s15.6: ERROR-CODE packs a class digit (top 3 bits of the 3rd value byte)
	// and a two-digit number (4th byte) - matches TurnClientTests.cpp's own helper.
	std::uint32_t PackErrorCode(int code)
	{
		return (static_cast<std::uint32_t>(code / 100) << 8) | static_cast<std::uint32_t>(code % 100);
	}

	// A deliberately minimal fake TURN server: one raw ENet socket, no allocation
	// bookkeeping beyond the Allocate 401-then-authenticated-success handshake, and a
	// single relayed "port" that is this same socket. It exists to prove NatTraversal's
	// (and, since it fronts the allocation, TurnRelaySocket's) WIRING - that TurnClient's
	// own bytes travel out through TurnRelaySocket's own dedicated relay-facing socket
	// and its replies (and a relayed peer datagram) arrive back through it - not
	// TurnClient's protocol correctness, which TurnClientTests.cpp already covers against
	// a fake in-process SendFn with no socket at all.
	//
	// A real TURN server tells the two directions apart by SOURCE address, never by
	// guessing from a datagram's leading bytes: everything arriving from the allocating
	// client's own 5-tuple is either a STUN/TURN control message or ChannelData/a Send
	// indication addressed AT a peer, and everything from anywhere else is a peer's raw
	// application datagram addressed AT the client. This class does the same -
	// m_clientAddress, once known, doubles as "who is us" - and that single check is
	// what makes relaying genuinely bidirectional: without it, the client's own outbound
	// ChannelData (indistinguishable from a peer's raw datagram by content alone, once
	// both travel over the wire as plain bytes) looks exactly like a peer sending
	// directly, and gets wrapped up and bounced straight back to the client instead of
	// ever reaching the peer - a relay that does not relay in the outbound direction.
	class FakeTurnServer
	{
	public:
		FakeTurnServer(std::uint16_t port, std::string username, std::string password, std::string realm)
		      : m_port(port), m_username(std::move(username)), m_password(std::move(password)), m_realm(std::move(realm))
		{
			m_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
			REQUIRE(m_socket != ENET_SOCKET_NULL);
			ENetAddress address{};
			address.host = ENET_HOST_ANY;
			address.port = port;
			REQUIRE(enet_socket_bind(m_socket, &address) == 0);
			REQUIRE(enet_socket_set_option(m_socket, ENET_SOCKOPT_NONBLOCK, 1) == 0);
		}

		~FakeTurnServer()
		{
			enet_socket_destroy(m_socket);
		}

		FakeTurnServer(const FakeTurnServer&) = delete;
		FakeTurnServer& operator=(const FakeTurnServer&) = delete;

		// Services at most one datagram per call, exactly like a single Tick of
		// anything else in this file. `relayPeerToClient` gates whether a datagram from
		// a real peer (sending straight to the allocated port) gets wrapped and
		// forwarded on - false while only the Allocate handshake is being driven, so a
		// stray early datagram cannot be mistaken for relay traffic.
		void PumpOnce(bool relayPeerToClient)
		{
			std::array<std::uint8_t, 1500> recvBuf{};
			ENetBuffer buffer{};
			buffer.data = recvBuf.data();
			buffer.dataLength = recvBuf.size();
			ENetAddress from{};
			const int received = enet_socket_receive(m_socket, &from, &buffer, 1);
			if (received <= 0)
			{
				return;
			}
			const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(recvBuf.data()), static_cast<std::size_t>(received)};

			if (net::stun::LooksLikeStun(data))
			{
				// Only the client ever speaks STUN/TURN to this socket - a peer just
				// sends plain application bytes - so this is also where m_clientAddress
				// (the source-address check below relies on it) gets learned.
				m_clientAddress = from;
				m_clientKnown = true;
				const auto reader = net::stun::MessageReader::Parse(data);
				REQUIRE(reader.has_value());
				if (reader->GetMethod() == net::stun::Method::CreatePermission)
				{
					AnswerControl(from, *reader);
					return;
				}
				if (reader->GetMethod() == net::stun::Method::ChannelBind)
				{
					RememberChannelBinding(*reader);
					AnswerControl(from, *reader);
					return;
				}
				if (reader->GetMethod() == net::stun::Method::Send && reader->GetClass() == net::stun::MessageClass::Indication)
				{
					// The client's own outbound traffic while a channel bind is still
					// outstanding - see TurnClient::SendToPeer. Unwrap it and forward the
					// inner DATA to the peer it names, exactly like ChannelData below,
					// rather than treating it as a peer's own datagram just because it
					// is not a request.
					HandleSendIndicationFromClient(*reader);
					return;
				}
				REQUIRE(reader->GetMethod() == net::stun::Method::Allocate);
				SendAllocateReply(from, *reader);
				return;
			}

			// Not STUN: either the client's own ChannelData - sharing the same 5-tuple as
			// its control-plane STUN messages, exactly like RFC 5766 s2.1 - or a real
			// peer's raw application datagram. Told apart by source address (see the
			// class comment), never by the leading bytes alone: an ordinary ENet datagram
			// can legitimately start with a byte in ChannelData's 0x40-0x7F range (an
			// unsequenced-flagged command, for one), which would misclassify real peer
			// traffic the moment both directions share this one socket.
			if (m_clientKnown && AddressesEqual(from, m_clientAddress))
			{
				if (net::stun::LooksLikeChannelData(data))
				{
					ForwardChannelDataToPeer(data);
				}
				// Anything else from the client's own address is malformed/unexpected
				// control-plane noise - dropped rather than acted on, same as any other
				// hostile input this fake does not model.
				return;
			}

			if (relayPeerToClient)
			{
				WrapAndForward(from, data);
			}
		}

		[[nodiscard]] net::stun::Endpoint RelayedAddress() const
		{
			return net::stun::Endpoint{0x7F000001u, m_port}; // 127.0.0.1, this same socket
		}

		// True once this server has unwrapped at least one ChannelData message or Send
		// indication that arrived from the allocating client and forwarded its inner
		// payload on to a peer. The relay round-trip test below uses this to prove the
		// host's outbound bytes actually crossed the relay's own protocol, rather than a
		// raw datagram having slipped straight to the peer some other way.
		[[nodiscard]] bool SawRelayedClientTraffic() const
		{
			return m_sawClientRelayedTraffic;
		}

	private:
		struct ChannelRoute
		{
			std::uint16_t channel = 0;
			net::stun::Endpoint peer{};
		};

		static bool AddressesEqual(const ENetAddress& a, const ENetAddress& b)
		{
			return a.host == b.host && a.port == b.port;
		}

		// Shared by CreatePermission and ChannelBind: a bare success, carrying only
		// MESSAGE-INTEGRITY, is all either response ever carries - PermitRelayPeer below
		// triggers BOTH in sequence once the permission is confirmed (see
		// TurnClient::Tick's upkeep loop), so both must be answered or the client just
		// keeps retrying instead of the test proceeding. Answered but never enforced:
		// this fake exists to prove the wiring, not RFC 5766 s8/s11's permission/channel
		// timeout and refresh rules (TurnClientTests.cpp already exercises that state
		// machine in isolation).
		void AnswerControl(const ENetAddress& to, const net::stun::MessageReader& reader)
		{
			net::stun::MessageBuilder resp(reader.GetMethod(), net::stun::MessageClass::SuccessResponse, reader.GetTransactionId());
			resp.AppendMessageIntegrity(net::stun::crypto::LongTermKey(m_username, m_realm, m_password));
			const auto bytes = resp.Bytes();
			ENetBuffer sendBuf{};
			sendBuf.data = const_cast<std::uint8_t*>(bytes.data());
			sendBuf.dataLength = bytes.size();
			enet_socket_send(m_socket, &to, &sendBuf, 1);
		}

		void SendAllocateReply(const ENetAddress& to, const net::stun::MessageReader& reader)
		{
			std::vector<std::uint8_t> bytes;
			if (!m_challenged)
			{
				net::stun::MessageBuilder resp(net::stun::Method::Allocate, net::stun::MessageClass::ErrorResponse, reader.GetTransactionId());
				resp.AddU32(net::stun::Attribute::ErrorCode, PackErrorCode(401));
				resp.AddText(net::stun::Attribute::Realm, m_realm);
				resp.AddText(net::stun::Attribute::Nonce, "nonce-1");
				bytes.assign(resp.Bytes().begin(), resp.Bytes().end());
				m_challenged = true;
			}
			else
			{
				net::stun::MessageBuilder resp(net::stun::Method::Allocate, net::stun::MessageClass::SuccessResponse, reader.GetTransactionId());
				resp.AddU32(net::stun::Attribute::Lifetime, 600u);
				resp.AddXorAddress(net::stun::Attribute::XorRelayedAddress, RelayedAddress());
				resp.AppendMessageIntegrity(net::stun::crypto::LongTermKey(m_username, m_realm, m_password));
				bytes.assign(resp.Bytes().begin(), resp.Bytes().end());
			}
			ENetBuffer sendBuf{};
			sendBuf.data = bytes.data();
			sendBuf.dataLength = bytes.size();
			enet_socket_send(m_socket, &to, &sendBuf, 1);
		}

		// Forwards to m_clientAddress - see PumpOnce's own comment for why that, and
		// never a fixed address, is where a relayed peer's datagram must land.
		void WrapAndForward(const ENetAddress& peerFrom, std::span<const std::byte> payload)
		{
			net::stun::MessageBuilder indication(net::stun::Method::Data, net::stun::MessageClass::Indication, net::stun::MakeTransactionId());
			indication.AddXorAddress(net::stun::Attribute::XorPeerAddress, net::stun::Endpoint{ENET_NET_TO_HOST_32(peerFrom.host), peerFrom.port});
			indication.AddBytes(net::stun::Attribute::Data, payload);
			const auto bytes = indication.Bytes();
			ENetBuffer sendBuf{};
			sendBuf.data = const_cast<std::uint8_t*>(bytes.data());
			sendBuf.dataLength = bytes.size();
			enet_socket_send(m_socket, &m_clientAddress, &sendBuf, 1);
		}

		// RFC 5766 s14.1: CHANNEL-NUMBER packs the 16-bit channel in the top half of a
		// 4-byte attribute, with 16 reserved bits below it that MUST be zero - matches
		// TurnClient::BuildChannelBind's own encoding.
		void RememberChannelBinding(const net::stun::MessageReader& reader)
		{
			const auto channelAttr = reader.U32(net::stun::Attribute::ChannelNumber);
			const auto peerAttr = reader.XorAddress(net::stun::Attribute::XorPeerAddress);
			if (!channelAttr.has_value() || !peerAttr.has_value())
			{
				return; // malformed; the client will just retry, same as any dropped datagram
			}
			const auto channel = static_cast<std::uint16_t>(*channelAttr >> 16);
			for (ChannelRoute& route: m_channelRoutes)
			{
				if (route.channel == channel)
				{
					route.peer = *peerAttr;
					return;
				}
			}
			m_channelRoutes.push_back(ChannelRoute{channel, *peerAttr});
		}

		[[nodiscard]] std::optional<net::stun::Endpoint> FindChannelPeer(std::uint16_t channel) const
		{
			for (const ChannelRoute& route: m_channelRoutes)
			{
				if (route.channel == channel)
				{
					return route.peer;
				}
			}
			return std::nullopt;
		}

		// Unwraps a ChannelData message FROM the client (see PumpOnce's own
		// source-address check) and forwards the inner application payload to whichever
		// peer RememberChannelBinding associated with this channel number - RFC 5766
		// s11's own "silently discard[ed]" for an unbound channel, same as any other
		// dropped datagram.
		void ForwardChannelDataToPeer(std::span<const std::byte> data)
		{
			if (data.size() < net::stun::kChannelDataHeaderSize)
			{
				return;
			}
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
			const auto channel = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
			const auto length = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[2]) << 8) | bytes[3]);
			if (static_cast<std::size_t>(net::stun::kChannelDataHeaderSize) + length > data.size())
			{
				return;
			}
			const auto peer = FindChannelPeer(channel);
			if (!peer.has_value())
			{
				return;
			}
			m_sawClientRelayedTraffic = true;
			SendRawToPeer(*peer, data.subspan(net::stun::kChannelDataHeaderSize, length));
		}

		// RFC 5766 s10.4: a Send indication carries XOR-PEER-ADDRESS and DATA, both
		// unauthenticated (indications never carry credentials at all) - matches
		// TurnClient::BuildSendIndication's own encoding.
		void HandleSendIndicationFromClient(const net::stun::MessageReader& reader)
		{
			const auto peer = reader.XorAddress(net::stun::Attribute::XorPeerAddress);
			const auto payload = reader.Find(net::stun::Attribute::Data);
			if (!peer.has_value() || !payload.has_value())
			{
				return;
			}
			m_sawClientRelayedTraffic = true;
			SendRawToPeer(*peer, *payload);
		}

		void SendRawToPeer(const net::stun::Endpoint& peer, std::span<const std::byte> payload)
		{
			ENetAddress to{};
			to.host = ENET_HOST_TO_NET_32(peer.address);
			to.port = peer.port;
			ENetBuffer sendBuf{};
			sendBuf.data = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(payload.data()));
			sendBuf.dataLength = payload.size();
			enet_socket_send(m_socket, &to, &sendBuf, 1);
		}

		ENetSocket m_socket = ENET_SOCKET_NULL;
		std::uint16_t m_port = 24791;
		std::string m_username;
		std::string m_password;
		std::string m_realm;
		bool m_challenged = false;
		ENetAddress m_clientAddress{};
		bool m_clientKnown = false;
		std::vector<ChannelRoute> m_channelRoutes;
		bool m_sawClientRelayedTraffic = false;
	};
} // namespace

TEST_CASE("A TURN allocation travels out through TurnRelaySocket's own relay-facing socket and its answer comes back through it")
{
	net::NetworkSubsystem client;
	REQUIRE(client.Host(24790, 4));

	FakeTurnServer server(24791, "gamer1", "hunter2", "aethercore.test");

	REQUIRE(client.Traversal()->BeginRelay("127.0.0.1", 24791, "gamer1", "hunter2"));
	CHECK(client.Traversal()->RelayState() == net::TurnClient::State::Allocating);

	for (int i = 0; i < 200 && client.Traversal()->RelayState() != net::TurnClient::State::Allocated; ++i)
	{
		client.Traversal()->Tick(0.05f);
		client.Poll();
		server.PumpOnce(false);
	}

	REQUIRE(client.Traversal()->RelayState() == net::TurnClient::State::Allocated);
	const auto candidate = client.Traversal()->RelayedEndpoint();
	REQUIRE(candidate.has_value());
	CHECK(net::NatTraversal::FormatAddress(*candidate) == "127.0.0.1");
	CHECK(candidate->port == 24791);
}

TEST_CASE("A relayed session carries traffic both ways: peer datagrams reach the host, and the host's own replies reach the peer")
{
	net::NetworkSubsystem client;
	REQUIRE(client.Host(24793, 4));
	net::NetworkSubsystem peer;
	REQUIRE(peer.Host(24795, 4));

	FakeTurnServer server(24794, "gamer1", "hunter2", "aethercore.test");

	REQUIRE(client.Traversal()->BeginRelay("127.0.0.1", 24794, "gamer1", "hunter2"));
	for (int i = 0; i < 200 && client.Traversal()->RelayState() != net::TurnClient::State::Allocated; ++i)
	{
		client.Traversal()->Tick(0.05f);
		client.Poll();
		server.PumpOnce(false);
	}
	REQUIRE(client.Traversal()->RelayState() == net::TurnClient::State::Allocated);
	const auto relayedCandidate = client.Traversal()->RelayedEndpoint();
	REQUIRE(relayedCandidate.has_value());

	// The permission the server would refuse to relay without - see RFC 5766 s8. In
	// production NetTraversalSession::TickRelay installs this for every candidate the
	// punch already tried; here it is the one address this test's peer sends from.
	const auto peerCandidate = net::NatTraversal::ParseEndpoint("127.0.0.1", 24795);
	REQUIRE(peerCandidate.has_value());
	client.Traversal()->PermitRelayPeer(*peerCandidate);
	for (int i = 0; i < 50; ++i)
	{
		client.Traversal()->Tick(0.05f);
		client.Poll();
		server.PumpOnce(false);
	}

	// The peer dials the relayed candidate exactly like ConnectThrough dials any other
	// candidate - no TURN awareness needed on its side at all, which is the whole point
	// of publishing it as just another candidate (see NatTraversal.hpp).
	REQUIRE(peer.ConnectThrough(*relayedCandidate));

	bool hostConnected = false;
	bool peerConnected = false;
	bool hostSent = false;
	bool peerSent = false;
	net::ConnectionId hostSawPeer = net::kInvalidConnection;
	std::string hostReceived;
	std::string peerReceived;
	const std::string kToPeer = "hello from the host, over the relay";
	const std::string kToHost = "hello from the peer, over the relay";

	for (int i = 0; i < 300 && (hostReceived.empty() || peerReceived.empty()); ++i)
	{
		// Drives TurnRelaySocket::Tick, which unwraps a datagram waiting on the
		// relay-facing socket and forwards it to client's own ENet host over the
		// loopback bridge (TurnRelaySocket::PumpRelayToEnet) AND wraps whatever ENet's
		// own socket just sent and forwards THAT the other way
		// (TurnRelaySocket::PumpEnetToRelay) - the send-side half that made this class
		// necessary at all (see TurnRelaySocket.hpp's own class comment).
		server.PumpOnce(true);
		client.Traversal()->Tick(0.05f);
		client.Poll();
		peer.Poll();

		for (const net::NetEvent& event: client.Events())
		{
			if (event.kind == net::NetEvent::Kind::Connected)
			{
				hostConnected = true;
				hostSawPeer = event.peer;
			}
			else if (event.kind == net::NetEvent::Kind::Data && hostReceived.empty())
			{
				hostReceived.assign(reinterpret_cast<const char*>(event.data.data()), event.data.size());
			}
		}
		for (const net::NetEvent& event: peer.Events())
		{
			if (event.kind == net::NetEvent::Kind::Connected)
			{
				peerConnected = true;
			}
			else if (event.kind == net::NetEvent::Kind::Data && peerReceived.empty())
			{
				peerReceived.assign(reinterpret_cast<const char*>(event.data.data()), event.data.size());
			}
		}

		// Each side sends exactly once, right after it sees itself connected - not
		// every tick, or whichever payload arrives first would just be one of several
		// identical retransmits rather than a single round trip proving the path works.
		if (hostConnected && !hostSent)
		{
			client.Send(hostSawPeer, net::kChannelReliable, true, std::as_bytes(std::span<const char>{kToPeer.data(), kToPeer.size()}));
			client.Flush();
			hostSent = true;
		}
		if (peerConnected && !peerSent)
		{
			peer.Send(net::kInvalidConnection, net::kChannelReliable, true, std::as_bytes(std::span<const char>{kToHost.data(), kToHost.size()}));
			peer.Flush();
			peerSent = true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	// Proves the seam this wave decided on: TurnRelaySocket::ForwardToEnet hands the
	// unwrapped payload to client's own ENet host as an ordinary loopback datagram,
	// sourced from 127.0.0.1:<ProxyPort()> - and ENet's OWN protocol handling, not
	// anything this class calls directly, is what turns that into a Connected event.
	CHECK(hostConnected);
	CHECK(peerConnected);

	// The bug this whole class exists to fix is exactly a relay that only ever carries
	// traffic ONE way - ENet has no send-side hook, so a relay that only unwraps
	// INBOUND peer datagrams still lets a symmetric NAT swallow every reply, and the
	// handshake above would have "succeeded" on a path that cannot carry anything the
	// host ever sends back. So both application payloads, not just CONNECT/VERIFY_CONNECT,
	// have to have actually crossed the wire.
	CHECK(hostReceived == kToHost);
	CHECK(peerReceived == kToPeer);

	// And it has to have gone through the relay's own protocol - ChannelData or a Send
	// indication - not slipped straight to the peer some other way, which would still
	// make `peerReceived` correct above while leaving the relay itself untested.
	CHECK(server.SawRelayedClientTraffic());
}
