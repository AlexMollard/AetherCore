#include "net/NetSession.hpp"

#include <algorithm>

namespace aether::net
{
	void NetSession::Bind(std::uint32_t netId, Entity entity)
	{
		// Both directions are REPLACED, not merged. Binding over an existing pair
		// used to strand the loser's entry in the opposite map: NetIdFor would keep
		// returning an id that EntityFor resolved to somebody else, and a later
		// unbind of either left the other dangling until the receive system's sweep
		// noticed. No production caller can reach this (spawn ids are never reused
		// and ApplySpawn unbinds a collision first), but Bind is public API and one
		// wrong call away.
		if (const auto forward = m_netIdByEntity.find(entity.id); forward != m_netIdByEntity.end()
		        && forward->second != netId)
		{
			m_byNetId.erase(forward->second);
		}
		if (const auto reverse = m_byNetId.find(netId); reverse != m_byNetId.end() && reverse->second.id != entity.id)
		{
			m_netIdByEntity.erase(reverse->second.id);
		}
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
		// The scene-placed derivation counter restarts at 1 - that determinism is the
		// whole reason this exists (see the header). The spawn counter does NOT: a
		// reused spawn id would alias a late packet onto a live entity.
		m_nextSceneNetId = 1;
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
		m_nextNetId = kSpawnNetIdBase;
		m_nextSceneNetId = 1;
		m_byNetId.clear();
		m_netIdByEntity.clear();
		m_connections.clear();
	}
} // namespace aether::net
