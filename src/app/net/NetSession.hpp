#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "net/NetTypes.hpp"
#include "scene/Entity.hpp"

namespace aether::net
{
	// Who we are on the network and what net ids map to what entities. Holds no
	// sockets - the transport is separate - so it is trivially testable.
	class NetSession
	{
	public:
		[[nodiscard]] NetRole Role() const
		{
			return m_role;
		}

		void SetRole(NetRole role)
		{
			m_role = role;
		}

		[[nodiscard]] bool IsHost() const
		{
			return m_role == NetRole::Host;
		}

		[[nodiscard]] ConnectionId LocalConnection() const
		{
			return m_localConnection;
		}

		void SetLocalConnection(ConnectionId id)
		{
			m_localConnection = id;
		}

		// Host-only. Ids are never reused within a session, so a late packet
		// referencing a despawned entity resolves to nothing rather than to
		// whatever entity happened to reuse the id.
		std::uint32_t AllocateNetId()
		{
			return m_nextNetId++;
		}

		void Bind(std::uint32_t netId, Entity entity);
		void Unbind(std::uint32_t netId);

		// Forget every binding and start allocating ids from 1 again, WITHOUT ending the
		// session: the role, the local connection id and the connection list all survive.
		//
		// Exists for one caller - a client that has changed scene since it was welcomed
		// (see NetworkContext::SetReplicationReady). The scene-placed derivation is
		// deterministic only because both ends walk the scene's node ids from the same
		// starting counter, so re-deriving in a new scene against a counter the previous
		// scene already advanced would number the same entity differently on the two
		// peers - silently, and with no packet involved to disagree about.
		void ResetBindings();

		[[nodiscard]] Entity EntityFor(std::uint32_t netId) const;
		[[nodiscard]] std::uint32_t NetIdFor(Entity entity) const;

		// Every live binding. A binding outlives its entity whenever the entity dies
		// by a route replication never sees - a scene load, a script's Entity.Destroy -
		// and EntityFor would then hand a caller a dangling handle to try_get with, so
		// the driving system sweeps these against the registry each tick.
		[[nodiscard]] const std::unordered_map<std::uint32_t, Entity>& Bindings() const
		{
			return m_byNetId;
		}

		void AddConnection(ConnectionId id);
		void RemoveConnection(ConnectionId id);

		[[nodiscard]] const std::vector<ConnectionId>& Connections() const
		{
			return m_connections;
		}

		void Clear();

	private:
		NetRole m_role = NetRole::Offline;
		ConnectionId m_localConnection = kInvalidConnection;
		std::uint32_t m_nextNetId = 1;
		std::unordered_map<std::uint32_t, Entity> m_byNetId;
		std::unordered_map<std::uint32_t, std::uint32_t> m_netIdByEntity; // Entity::id -> netId
		std::vector<ConnectionId> m_connections;
	};
} // namespace aether::net
