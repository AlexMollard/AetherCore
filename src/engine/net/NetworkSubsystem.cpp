#include "net/NetworkSubsystem.hpp"

#include <cstring>

#include <enet/enet.h>

#include "net/EnetInit.hpp"
#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// The flag for a non-reliable send. ENet's default (no flags) is already
		// unreliable-SEQUENCED: the receiving channel drops a packet whose sequence
		// number is older than one it has already delivered, which is exactly what
		// kChannelSnapshot is documented to give (NetTypes.hpp). UNRELIABLE_FRAGMENT
		// adds the one thing the default lacks - a payload over the MTU is fragmented
		// unreliably instead of being silently promoted to a reliable, retransmitted,
		// head-of-line-blocking transfer. A snapshot carries ~12 bytes per changed
		// field across the whole relevant set, so exceeding the MTU is routine.
		//
		// ENET_PACKET_FLAG_UNSEQUENCED would be WRONG here and is a different flag
		// entirely: it bypasses the channel's sequence check, so a reordered snapshot
		// is delivered and applied. Combined with per-field change detection that
		// never resends an unchanged value, a stale field applied out of order stays
		// wrong until that field next changes - permanently, in the common case.
		constexpr enet_uint32 kUnreliableSequenced = ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	} // namespace

	NetworkSubsystem::~NetworkSubsystem()
	{
		Disconnect();
	}

	bool NetworkSubsystem::Host(std::uint16_t port, int maxPeers)
	{
		Disconnect();
		if (!AcquireEnet())
		{
			m_lastError = "enet_initialize failed";
			return false;
		}
		m_enetAcquired = true;

		ENetAddress address{};
		address.host = ENET_HOST_ANY;
		address.port = port;
		m_host = enet_host_create(&address, static_cast<std::size_t>(maxPeers), kChannelCount, 0, 0);
		if (m_host == nullptr)
		{
			m_lastError = "enet_host_create failed (port in use?)";
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}
		m_traversal = std::make_unique<NatTraversal>(m_host);
		m_role = NetRole::Host;
		m_localId = kInvalidConnection;
		m_nextPeerId = 1;
		AE_INFO(LogCategory::App, "Hosting on port {}", port);
		return true;
	}

	bool NetworkSubsystem::Connect(std::string_view host, std::uint16_t port)
	{
		Disconnect();
		if (!AcquireEnet())
		{
			m_lastError = "enet_initialize failed";
			return false;
		}
		m_enetAcquired = true;

		m_host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
		if (m_host == nullptr)
		{
			m_lastError = "enet_host_create failed";
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}

		ENetAddress address{};
		const std::string hostStr{host};
		if (enet_address_set_host(&address, hostStr.c_str()) != 0)
		{
			m_lastError = "could not resolve '" + hostStr + "'";
			enet_host_destroy(m_host);
			m_host = nullptr;
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}
		address.port = port;

		m_serverPeer = enet_host_connect(m_host, &address, kChannelCount, 0);
		if (m_serverPeer == nullptr)
		{
			m_lastError = "no available peers";
			enet_host_destroy(m_host);
			m_host = nullptr;
			ReleaseEnet();
			m_enetAcquired = false;
			return false;
		}
		m_traversal = std::make_unique<NatTraversal>(m_host);
		m_role = NetRole::Client;
		return true;
	}

	void NetworkSubsystem::Disconnect()
	{
		// Before the host, always: its destructor clears the intercept it installed, and
		// a callback left pointing at freed state fires on the next datagram.
		m_traversal.reset();
		if (m_host != nullptr)
		{
			if (m_serverPeer != nullptr)
			{
				enet_peer_disconnect_now(m_serverPeer, 0);
				m_serverPeer = nullptr;
			}
			// A HOST DELIBERATELY DOES NOT DISCONNECT ITS PEERS HERE, and that is not an
			// omission. ENet's disconnect handler begins with enet_peer_reset_queues,
			// which throws away every packet the receiver has taken off the wire but not
			// yet dispatched - so a disconnect sent microseconds behind a message (which
			// is what "broadcast a goodbye, then shut down" means) arrives in the same
			// service call at the far end and destroys the goodbye before the
			// application ever sees it. The message a host quits with matters more than
			// the handshake: a client that receives it tears its own session down at
			// once, which is prompter than being disconnected. One that misses it falls
			// back to the transport timeout, which is exactly what a crashed host looks
			// like - and is what it should be treated as.
			//
			// A host dropping ONE peer is a different case and does disconnect it: see
			// DisconnectPeer, which can defer the drop because the host stays alive to
			// finish sending first.
			enet_host_destroy(m_host);
			m_host = nullptr;
		}
		if (m_enetAcquired)
		{
			ReleaseEnet();
			m_enetAcquired = false;
		}
		m_role = NetRole::Offline;
		m_localId = kInvalidConnection;
		m_events.clear();
	}

	void NetworkSubsystem::DisconnectPeer(ConnectionId peer)
	{
		if (m_host == nullptr || m_role != NetRole::Host)
		{
			return;
		}
		if (ENetPeer* target = PeerFor(peer))
		{
			enet_peer_disconnect_later(target, 0);
		}
	}

	void NetworkSubsystem::Flush()
	{
		if (m_host != nullptr)
		{
			enet_host_flush(m_host);
		}
	}

	_ENetPeer* NetworkSubsystem::PeerFor(ConnectionId id) const
	{
		if (m_host == nullptr)
		{
			return nullptr;
		}
		for (std::size_t i = 0; i < m_host->peerCount; ++i)
		{
			ENetPeer* peer = &m_host->peers[i];
			if (peer->state == ENET_PEER_STATE_CONNECTED && static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(peer->data)) == id)
			{
				return peer;
			}
		}
		return nullptr;
	}

	std::uint32_t NetworkSubsystem::RoundTripMs(ConnectionId peer) const
	{
		if (m_host == nullptr)
		{
			return 0;
		}
		const ENetPeer* target = m_role == NetRole::Client ? m_serverPeer : PeerFor(peer);
		if (target == nullptr || target->state != ENET_PEER_STATE_CONNECTED)
		{
			return 0;
		}
		return static_cast<std::uint32_t>(target->roundTripTime);
	}

	void NetworkSubsystem::Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_host == nullptr || bytes.empty() || channel < 0 || channel >= kChannelCount)
		{
			return;
		}
		ENetPeer* target = m_role == NetRole::Client ? m_serverPeer : PeerFor(peer);
		if (target == nullptr)
		{
			return;
		}
		ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
		        reliable ? ENET_PACKET_FLAG_RELIABLE : kUnreliableSequenced);
		// enet_peer_send only takes ownership of the packet on success; on failure
		// (bad channel, oversized payload, allocation failure) it leaves the packet
		// with a zero refcount for us to free, or it leaks.
		if (enet_peer_send(target, static_cast<enet_uint8>(channel), packet) != 0)
		{
			enet_packet_destroy(packet);
		}
	}

	void NetworkSubsystem::Broadcast(int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_host == nullptr || bytes.empty() || channel < 0 || channel >= kChannelCount)
		{
			return;
		}
		ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
		        reliable ? ENET_PACKET_FLAG_RELIABLE : kUnreliableSequenced);
		// enet_host_broadcast frees the packet itself when no peer accepts it, unlike
		// enet_peer_send - do not destroy it here.
		enet_host_broadcast(m_host, static_cast<enet_uint8>(channel), packet);
	}

	void NetworkSubsystem::Poll()
	{
		m_events.clear();
		if (m_host == nullptr)
		{
			return;
		}

		ENetEvent event{};
		while (enet_host_service(m_host, &event, 0) > 0)
		{
			switch (event.type)
			{
			case ENET_EVENT_TYPE_NONE:
				break;
			case ENET_EVENT_TYPE_CONNECT:
			{
				// The host stamps each peer with a small stable id; the client's single
				// peer is the server and never needs one.
				const ConnectionId id = m_role == NetRole::Host ? m_nextPeerId++ : kInvalidConnection;
				event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
				m_events.push_back(NetEvent{.kind = NetEvent::Kind::Connected, .peer = id});
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				const auto id = static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(event.peer->data));
				event.peer->data = nullptr;
				m_events.push_back(NetEvent{.kind = NetEvent::Kind::Disconnected, .peer = id});
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
			{
				const auto id = static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(event.peer->data));
				NetEvent e{.kind = NetEvent::Kind::Data, .peer = id, .channel = event.channelID};
				e.data.resize(event.packet->dataLength);
				std::memcpy(e.data.data(), event.packet->data, event.packet->dataLength);
				m_events.push_back(std::move(e));
				enet_packet_destroy(event.packet);
				break;
			}
			default:
				break;
			}
		}
	}
} // namespace aether::net
