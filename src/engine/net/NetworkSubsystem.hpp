#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/NetTypes.hpp"

struct _ENetHost;
struct _ENetPeer;

namespace aether::net
{
	// ENet transport. Deliberately knows nothing about entities, components or
	// replication - it moves bytes between peers and reports connection events.
	// The replication layer in src/app/net sits on top of this.
	//
	// Polled on the MAIN thread from the network systems, not on a worker: unlike
	// the editor's ControlServer (which hands work across a queue and touches
	// nothing live), replication reads and writes the ECS every tick.
	class NetworkSubsystem
	{
	public:
		NetworkSubsystem() = default;
		~NetworkSubsystem();

		NetworkSubsystem(const NetworkSubsystem&) = delete;
		NetworkSubsystem& operator=(const NetworkSubsystem&) = delete;

		bool Host(std::uint16_t port, int maxPeers);
		bool Connect(std::string_view host, std::uint16_t port);
		void Disconnect();

		void Send(ConnectionId peer, int channel, bool reliable, std::span<const std::byte> bytes);
		void Broadcast(int channel, bool reliable, std::span<const std::byte> bytes);

		// Drains ENet into Events(). Call once per frame before reading events;
		// each call clears the previous frame's events.
		void Poll();

		[[nodiscard]] std::span<const NetEvent> Events() const
		{
			return m_events;
		}

		[[nodiscard]] NetRole Role() const
		{
			return m_role;
		}

		[[nodiscard]] bool IsActive() const
		{
			return m_role != NetRole::Offline;
		}

		// On a client, the id the host assigned us (0 until connected). On a host, 0.
		[[nodiscard]] ConnectionId LocalConnectionId() const
		{
			return m_localId;
		}

		void SetLocalConnectionId(ConnectionId id)
		{
			m_localId = id;
		}

		[[nodiscard]] std::string LastError() const
		{
			return m_lastError;
		}

	private:
		_ENetPeer* PeerFor(ConnectionId id) const;

		_ENetHost* m_host = nullptr;
		_ENetPeer* m_serverPeer = nullptr; // client only
		NetRole m_role = NetRole::Offline;
		ConnectionId m_localId = kInvalidConnection;
		ConnectionId m_nextPeerId = 1;
		std::vector<NetEvent> m_events;
		std::string m_lastError;
		bool m_enetAcquired = false;
	};
} // namespace aether::net
