#include "net/NetSession.hpp"

#include <algorithm>

namespace aether::net
{
	void NetSession::Bind(std::uint32_t netId, Entity entity)
	{
		m_byNetId[netId] = entity;
		m_netIdByEntity[entity.id] = netId;
	}

	void NetSession::Unbind(std::uint32_t netId)
	{
		if (const auto it = m_byNetId.find(netId); it != m_byNetId.end())
		{
			m_netIdByEntity.erase(it->second.id);
			m_byNetId.erase(it);
		}
	}

	Entity NetSession::EntityFor(std::uint32_t netId) const
	{
		const auto it = m_byNetId.find(netId);
		return it == m_byNetId.end() ? Entity{} : it->second;
	}

	std::uint32_t NetSession::NetIdFor(Entity entity) const
	{
		const auto it = m_netIdByEntity.find(entity.id);
		return it == m_netIdByEntity.end() ? 0u : it->second;
	}

	void NetSession::ResetBindings()
	{
		m_nextNetId = 1;
		m_byNetId.clear();
		m_netIdByEntity.clear();
	}

	void NetSession::AddConnection(ConnectionId id)
	{
		if (std::find(m_connections.begin(), m_connections.end(), id) == m_connections.end())
		{
			m_connections.push_back(id);
		}
	}

	void NetSession::RemoveConnection(ConnectionId id)
	{
		m_connections.erase(std::remove(m_connections.begin(), m_connections.end(), id), m_connections.end());
	}

	void NetSession::Clear()
	{
		m_role = NetRole::Offline;
		m_localConnection = kInvalidConnection;
		m_nextNetId = 1;
		m_byNetId.clear();
		m_netIdByEntity.clear();
		m_connections.clear();
	}
} // namespace aether::net
