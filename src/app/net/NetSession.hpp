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

		[[nodiscard]] Entity EntityFor(std::uint32_t netId) const;
		[[nodiscard]] std::uint32_t NetIdFor(Entity entity) const;

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
