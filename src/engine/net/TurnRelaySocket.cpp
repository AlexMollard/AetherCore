#include "net/TurnRelaySocket.hpp"

#include <algorithm>
#include <array>

#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// 127.0.0.1, in the HOST-order convention TurnClient::Endpoint (== stun::Endpoint)
		// uses throughout - see StunMessage.hpp's own Endpoint comment.
		constexpr std::uint32_t kLoopbackHostOrder = 0x7F000001u;

		// The largest datagram ENet itself ever sends or reads in one piece - see
		// ENetHost::packetData's own sizing in enet.h. Bounds the loopback leg: anything
		// bigger than this could not have come from ENet's own socket honestly, and
		// nothing this class unwraps from the relay is ever handed to ENet past this
		// size either.
		constexpr std::size_t kMaxEnetDatagram = ENET_PROTOCOL_MAXIMUM_MTU;

		// The relay leg carries kMaxEnetDatagram's worth of application payload PLUS
		// TURN's own framing - a 20-byte STUN/TURN header, XOR-PEER-ADDRESS and DATA
		// attributes (each padded to a 4-byte boundary) for a Send indication, or just
		// the 4-byte ChannelData header once a channel is bound - plus MESSAGE-INTEGRITY
		// on anything authenticated. 256 bytes of headroom covers all of that with room
		// to spare.
		constexpr std::size_t kMaxRelayDatagram = kMaxEnetDatagram + 256;

		ENetAddress ToEnetAddress(const TurnClient::Endpoint& e)
		{
			ENetAddress address{};
			address.host = ENET_HOST_TO_NET_32(e.address);
			address.port = e.port;
			return address;
		}

		TurnClient::Endpoint FromEnetAddress(const ENetAddress& a)
		{
			return TurnClient::Endpoint{ENET_NET_TO_HOST_32(a.host), a.port};
		}

		// Binds `socket` to the given address and puts it in non-blocking mode - the
		// shape every socket this class opens needs, whether loopback-only or ANY-bound.
		bool BindNonBlocking(ENetSocket socket, const ENetAddress& address)
		{
			return enet_socket_bind(socket, &address) == 0 && enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1) == 0;
		}
	} // namespace

	TurnRelaySocket::TurnRelaySocket(std::uint16_t enetLoopbackPort, TurnClient::Endpoint server, std::string username, std::string password)
	      : m_enetLoopbackPort(enetLoopbackPort)
	      , m_turn(server, std::move(username), std::move(password),
	                [this](const TurnClient::Endpoint& to, std::span<const std::uint8_t> bytes) { SendToRelay(to, bytes); })
	{
		// The loopback leg: bound to 127.0.0.1 specifically, NEVER ENET_HOST_ANY - this
		// is the internal bridge to ENet's own socket, and binding it wide open would
		// let any process on this machine feed it traffic to relay as though it were
		// ENet's own, which PumpEnetToRelay's own source check below exists to catch
		// even if this bind were ever loosened by mistake.
		m_loopbackSocket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
		if (m_loopbackSocket != ENET_SOCKET_NULL)
		{
			ENetAddress loopback{};
			enet_address_set_host_ip(&loopback, "127.0.0.1"); // a literal dotted quad; cannot fail
			loopback.port = 0;                                // ephemeral; read back below
			if (BindNonBlocking(m_loopbackSocket, loopback))
			{
				ENetAddress bound{};
				if (enet_socket_get_address(m_loopbackSocket, &bound) == 0)
				{
					m_proxyPort = bound.port;
				}
				else
				{
					enet_socket_destroy(m_loopbackSocket);
					m_loopbackSocket = ENET_SOCKET_NULL;
				}
			}
			else
			{
				enet_socket_destroy(m_loopbackSocket);
				m_loopbackSocket = ENET_SOCKET_NULL;
			}
		}

		// The relay leg: an ordinary ANY-bound socket, because it has to reach a real
		// address on the internet - a socket bound to 127.0.0.1 above cannot (the kernel
		// has no route for a loopback-sourced packet toward a non-loopback destination).
		m_relaySocket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
		if (m_relaySocket != ENET_SOCKET_NULL)
		{
			ENetAddress any{};
			any.host = ENET_HOST_ANY;
			any.port = 0;
			if (!BindNonBlocking(m_relaySocket, any))
			{
				enet_socket_destroy(m_relaySocket);
				m_relaySocket = ENET_SOCKET_NULL;
			}
		}
	}

	TurnRelaySocket::~TurnRelaySocket()
	{
		m_turn.Release();
		if (m_loopbackSocket != ENET_SOCKET_NULL)
		{
			enet_socket_destroy(m_loopbackSocket);
		}
		if (m_relaySocket != ENET_SOCKET_NULL)
		{
			enet_socket_destroy(m_relaySocket);
		}
	}

	void TurnRelaySocket::BeginAllocate()
	{
		m_turn.BeginAllocate();
	}

	void TurnRelaySocket::Release()
	{
		m_turn.Release();
	}

	void TurnRelaySocket::PermitPeer(const TurnClient::Endpoint& peer)
	{
		m_turn.PermitPeer(peer);
		if (std::ranges::find(m_peers, peer) == m_peers.end())
		{
			m_peers.push_back(peer);
		}
	}

	void TurnRelaySocket::SendToRelay(const TurnClient::Endpoint& to, std::span<const std::uint8_t> bytes)
	{
		if (m_relaySocket == ENET_SOCKET_NULL)
		{
			return;
		}
		const ENetAddress address = ToEnetAddress(to);
		ENetBuffer buffer{};
		buffer.data = const_cast<std::uint8_t*>(bytes.data());
		buffer.dataLength = bytes.size();
		enet_socket_send(m_relaySocket, &address, &buffer, 1);
	}

	void TurnRelaySocket::ForwardToEnet(std::span<const std::byte> payload)
	{
		// Truncated (nothing to deliver) or bigger than ENet itself ever sends in one
		// datagram - either way dropped rather than handed to ENet, which would either
		// do nothing useful with an empty packet or choke on one its own protocol never
		// produces.
		if (payload.empty() || payload.size() > kMaxEnetDatagram)
		{
			return;
		}
		ENetAddress to{};
		to.host = ENET_HOST_TO_NET_32(kLoopbackHostOrder);
		to.port = m_enetLoopbackPort;
		ENetBuffer buffer{};
		buffer.data = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(payload.data()));
		buffer.dataLength = payload.size();
		// Sent from m_loopbackSocket - not m_relaySocket - so the packet's SOURCE
		// address is 127.0.0.1:ProxyPort(), exactly what ENet was told the peer's
		// address is (see NatTraversal::RelayConnectEndpoint). Any other source and
		// ENet would treat this as an unrecognised, unsolicited sender instead of the
		// peer it is already expecting.
		enet_socket_send(m_loopbackSocket, &to, &buffer, 1);
	}

	void TurnRelaySocket::PumpEnetToRelay()
	{
		// One byte past the real cap: a receive that fills this ENTIRE buffer signals a
		// datagram at least that large arrived, which is impossible for ENet's own
		// traffic and is checked for explicitly below rather than assumed away.
		std::array<std::uint8_t, kMaxEnetDatagram + 1> buffer{};
		for (;;)
		{
			ENetAddress from{};
			ENetBuffer recvBuffer{};
			recvBuffer.data = buffer.data();
			recvBuffer.dataLength = buffer.size();
			const int received = enet_socket_receive(m_loopbackSocket, &from, &recvBuffer, 1);
			if (received <= 0)
			{
				return; // nothing left waiting this Tick, or a transient error either way
			}
			if (static_cast<std::size_t>(received) > kMaxEnetDatagram)
			{
				continue; // oversized; dropped, keep draining whatever else is queued
			}
			// Only ENet's own loopback bind, never anyone else on this machine - see the
			// class comment on why accepting an arbitrary local sender here would let it
			// ride this relay allocation as an open forwarder.
			if (ENET_NET_TO_HOST_32(from.host) != kLoopbackHostOrder || from.port != m_enetLoopbackPort)
			{
				AE_WARN(LogCategory::App, "TURN relay proxy: dropped a datagram on the loopback bridge from an unexpected source");
				continue;
			}
			const std::span<const std::byte> payload{reinterpret_cast<const std::byte*>(buffer.data()), static_cast<std::size_t>(received)};
			for (const TurnClient::Endpoint& peer: m_peers)
			{
				m_turn.SendToPeer(peer, payload);
			}
		}
	}

	void TurnRelaySocket::PumpRelayToEnet()
	{
		std::array<std::uint8_t, kMaxRelayDatagram + 1> buffer{};
		for (;;)
		{
			ENetAddress from{};
			ENetBuffer recvBuffer{};
			recvBuffer.data = buffer.data();
			recvBuffer.dataLength = buffer.size();
			const int received = enet_socket_receive(m_relaySocket, &from, &recvBuffer, 1);
			if (received <= 0)
			{
				return;
			}
			if (static_cast<std::size_t>(received) > kMaxRelayDatagram)
			{
				continue; // oversized; dropped rather than parsed out of an over-full buffer
			}
			const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(buffer.data()), static_cast<std::size_t>(received)};
			// TurnClient itself is what validates this is really from the configured
			// server and, if so, whether it is a well-formed ChannelData/Data payload -
			// see TurnClient::OnDatagram's own hostile-input handling. Anything else
			// (garbage, a truncated frame, an unexpected sender) comes back as either
			// consumed=false or consumed=true with no peer, and is dropped here either
			// way rather than forwarded on a guess.
			const TurnClient::Delivery delivery = m_turn.OnDatagram(FromEnetAddress(from), data);
			if (delivery.consumed && delivery.peer.has_value())
			{
				ForwardToEnet(delivery.payload);
			}
		}
	}

	void TurnRelaySocket::Tick(float deltaSeconds)
	{
		m_turn.Tick(deltaSeconds);
		PumpEnetToRelay();
		PumpRelayToEnet();
	}
} // namespace aether::net
