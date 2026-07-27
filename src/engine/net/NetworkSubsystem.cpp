#include "net/NetworkSubsystem.hpp"

#include <cstring>
#include <mutex>

#include <enet/enet.h>

#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// ENet's global init is process-wide and not refcounted by the library.
		// Same pattern as editor/ControlServer.cpp so two hosts in one process
		// (loopback tests, editor + game) cannot tear each other down.
		std::mutex g_enetInitMutex;
		int g_enetRefCount = 0;

		bool AcquireEnet()
		{
			const std::lock_guard<std::mutex> lock(g_enetInitMutex);
			if (g_enetRefCount == 0 && enet_initialize() != 0)
			{
				return false;
			}
			++g_enetRefCount;
			return true;
		}

		void ReleaseEnet()
		{
			const std::lock_guard<std::mutex> lock(g_enetInitMutex);
			if (g_enetRefCount > 0 && --g_enetRefCount == 0)
			{
				enet_deinitialize();
			}
		}
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
		m_role = NetRole::Client;
		return true;
	}

	void NetworkSubsystem::Disconnect()
	{
		if (m_host != nullptr)
		{
			if (m_serverPeer != nullptr)
			{
				enet_peer_disconnect_now(m_serverPeer, 0);
				m_serverPeer = nullptr;
			}
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

	void NetworkSubsystem::Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_host == nullptr || bytes.empty())
		{
			return;
		}
		ENetPeer* target = m_role == NetRole::Client ? m_serverPeer : PeerFor(peer);
		if (target == nullptr)
		{
			return;
		}
		ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
		        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
		enet_peer_send(target, static_cast<enet_uint8>(channel), packet);
	}

	void NetworkSubsystem::Broadcast(int channel, bool reliable, std::span<const std::byte> bytes)
	{
		if (m_host == nullptr || bytes.empty())
		{
			return;
		}
		ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
		        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
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
