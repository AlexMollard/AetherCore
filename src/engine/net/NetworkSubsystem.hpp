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

		// Drops ONE peer (host only), after everything already queued for it has been
		// sent. `enet_peer_disconnect_later` rather than `_now` is the whole point: the
		// caller's last word to that peer - the reason it is being dropped - is a
		// reliable packet sitting in the outgoing queue, and `_now` would tear the link
		// down on top of it.
		void DisconnectPeer(ConnectionId peer);

		// Pushes everything queued onto the wire immediately instead of waiting for the
		// next Poll. Needed exactly once: a peer that is about to destroy its host has
		// no next Poll, so a goodbye broadcast would never leave the process.
		void Flush();

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

		// Round-trip time to `peer`, in milliseconds, or 0 when there is no such link.
		//
		// ENet already measures this: every reliable packet is acknowledged, and the
		// peer keeps a smoothed round-trip estimate from those acknowledgements. Taking
		// it is strictly better than a ping message of our own would be - it costs no
		// packets at all, it is averaged over real traffic rather than over a probe that
		// competes with it, and it cannot disagree with the transport's own idea of the
		// link.
		//
		// On a CLIENT there is exactly one link and `peer` is ignored, matching how
		// Send() addresses the host. On a HOST it names the connection to measure; the
		// host's link to itself is not a link and reports 0.
		[[nodiscard]] std::uint32_t RoundTripMs(ConnectionId peer) const;

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
