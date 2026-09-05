#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "net/NetTypes.hpp"
#include "scene/Entity.hpp"

namespace aether::net
{
	// The net-id space is split in two so the two ways an id can come to exist can
	// never collide. Scene-placed ids are DERIVED identically on every peer from 1
	// upward (AssignScenePlacedNetIds walks the scene file's node ids), while
	// spawned ids come from the host's counter alone. A client that becomes
	// replication-ready in a scene with more replicated scene entities than the
	// host has ever allocated for must not have derived an id the host is about to
	// hand a spawn - which one shared counter made possible (SetReplicationReady
	// resets the client's derivation counter to 1). One million scene-placed ids
	// per scene is the ceiling this reserves; AllocateSceneNetId returns 0 past it.
	inline constexpr std::uint32_t kSpawnNetIdBase = 0x0010'0000u;

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

		// Host-only, for session-spawned entities: ids start at kSpawnNetIdBase,
		// above every id AssignScenePlacedNetIds can derive on ANY peer, so a spawn
		// can never land on an id a client already holds. Ids are never reused
		// within a session, so a late packet referencing a despawned entity resolves
		// to nothing rather than to whatever entity happened to reuse the id.
		std::uint32_t AllocateNetId()
		{
			// The exhaustion guard AllocateSceneNetId has always had, for the spawn
			// half: past UINT32_MAX the counter wraps to 0 - which every decoder reads
			// as "no id" - and then walks up through the scene-placed space the split
			// below exists to keep disjoint, colliding with ids clients derive from
			// their scene files. Return 0 and let the caller leave the entity unbound,
			// exactly as scene-space exhaustion does.
			if (m_nextNetId < kSpawnNetIdBase) // wrapped past UINT32_MAX
			{
				return 0;
			}
			return m_nextNetId++;
		}

		// The scene-placed half of the id space: 1..kSpawnNetIdBase-1, assigned in
		// SceneNodeComponent::id order by AssignScenePlacedNetIds. Both peers start
		// this counter at 1, which is what makes the derivation agree with no
		// handshake. Returns 0 when the scene-placed space is exhausted - the caller
		// leaves that entity unbound rather than reaching into the spawn space.
		std::uint32_t AllocateSceneNetId()
		{
			if (m_nextSceneNetId >= kSpawnNetIdBase)
			{
				return 0;
			}
			return m_nextSceneNetId++;
		}

		[[nodiscard]] ConnectionId LocalConnection() const
		{
			return m_localConnection;
		}

		void SetLocalConnection(ConnectionId id)
		{
			m_localConnection = id;
		}

		void Bind(std::uint32_t netId, Entity entity);
		void Unbind(std::uint32_t netId);

		// Forget every binding and start the scene-placed derivation counter at 1 again,
		// WITHOUT ending the session: the role, the local connection id and the connection
		// list all survive. The spawn counter is NOT rewound - ids are never reused within
		// a session.
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
		// Stages allocator states the public API cannot reach (id-space
		// exhaustion); see NetSessionTests.cpp.
		friend struct NetSessionTestPeer;
		NetRole m_role = NetRole::Offline;
		ConnectionId m_localConnection = kInvalidConnection;
		std::uint32_t m_nextNetId = kSpawnNetIdBase; // spawned ids only - never the scene-placed space
		std::uint32_t m_nextSceneNetId = 1;
		std::unordered_map<std::uint32_t, Entity> m_byNetId;
		std::unordered_map<std::uint32_t, std::uint32_t> m_netIdByEntity; // Entity::id -> netId
		std::vector<ConnectionId> m_connections;
	};
} // namespace aether::net
