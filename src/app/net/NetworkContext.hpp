#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetRelevancy.hpp"
#include "net/NetSession.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/ReplicationSchema.hpp"
#include "scene/Entity.hpp"
#include "scene/reflection/Reflection.hpp"

namespace aether
{
	class ServiceContainer;
	class World;
} // namespace aether

namespace aether::net
{
	class RpcBridge;
	class ScriptFieldBridge;

	// The one live networking object in a running app: the transport, the session,
	// the replication schema they share, and the tuning both network systems read.
	//
	// It exists because three separate consumers - NetworkReceiveSystem,
	// NetworkSendSystem and the Net.* script exports - must all drive the SAME
	// session, and a System cannot reach another System's members. Registering the
	// shared object in the ServiceContainer and resolving it with TryGet<> is the
	// established pattern here (RenderingSubsystem registers ui::FontRegistry the
	// same way for ScriptComponentSystem to pick up), so this follows it rather
	// than inventing a second one.
	//
	// Every accessor is safe to call with no session: a title screen asks
	// IsHost/IsConnected before anything has connected, and the two systems tick
	// every frame of an unnetworked game.
	class NetworkContext
	{
	public:
		explicit NetworkContext(ServiceContainer& services);
		~NetworkContext();

		NetworkContext(const NetworkContext&) = delete;
		NetworkContext& operator=(const NetworkContext&) = delete;
		NetworkContext(NetworkContext&&) = delete;
		NetworkContext& operator=(NetworkContext&&) = delete;

		// ── Session lifecycle ────────────────────────────────────────────────
		// All three take the world: the host's scene-placed entities get their
		// deterministic net ids the moment the session starts (before a single packet
		// moves), and Stop destroys the entities the session spawned.
		bool StartHost(World& world, std::uint16_t port, int maxPeers);
		bool StartClient(World& world, std::string_view host, std::uint16_t port);

		// Tears the session down AND destroys every replicated entity this session
		// spawned (NetworkIdentity with scenePlaced == false). Leaving them behind
		// makes the next join replay a duplicate of each one - see the note in the
		// definition.
		void Stop(World& world);

		[[nodiscard]] bool IsActive() const
		{
			return m_transport.IsActive();
		}

		[[nodiscard]] bool IsHost() const
		{
			return m_transport.Role() == NetRole::Host;
		}

		[[nodiscard]] bool IsClient() const
		{
			return m_transport.Role() == NetRole::Client;
		}

		// A host is connected the moment it is listening; a client only once the
		// host has answered with its Welcome and given it a connection id.
		[[nodiscard]] bool IsConnected() const
		{
			return IsHost() || (IsClient() && m_session.LocalConnection() != kInvalidConnection);
		}

		[[nodiscard]] ConnectionId LocalConnectionId() const
		{
			return m_session.LocalConnection();
		}

		[[nodiscard]] std::string LastError() const
		{
			return m_transport.LastError();
		}

		// ── Shared state ─────────────────────────────────────────────────────
		[[nodiscard]] NetworkSubsystem& Transport()
		{
			return m_transport;
		}

		[[nodiscard]] NetSession& Session()
		{
			return m_session;
		}

		[[nodiscard]] const ReplicationSchema& Schema() const
		{
			return m_schema;
		}

		[[nodiscard]] const std::vector<reflect::ComponentType>& Catalog() const;

		[[nodiscard]] RelevancySettings& Relevancy()
		{
			return m_relevancy;
		}

		// Snapshots per connection per second. State replication is rate-limited
		// rather than frame-locked: at 144 fps a frame-locked host would spend most
		// of its upstream retransmitting sub-millimetre motion no client can show.
		[[nodiscard]] float SendRateHz() const
		{
			return m_sendRateHz;
		}

		void SetSendRateHz(float hz)
		{
			m_sendRateHz = hz;
		}

		// One change-detection cache PER CONNECTION. A single shared cache would
		// record a field as sent the moment any one connection received it, so a
		// connection that only just became relevant to that entity would never be
		// told the field's current value.
		[[nodiscard]] SnapshotCache& CacheFor(ConnectionId connection)
		{
			return m_caches[connection];
		}

		void DropCacheFor(ConnectionId connection)
		{
			m_caches.erase(connection);
		}

		// Net ids are never reused, so a stale entry is only wasted memory - but a
		// long session that spawns and despawns constantly would grow every cache
		// without bound, so a despawn drops the entity from all of them.
		void ForgetNetId(std::uint32_t netId);

		[[nodiscard]] ServiceContainer& Services() const
		{
			return m_services;
		}

		// Seconds since the process started, from a monotonic clock rather than the
		// frame delta: interpolation is a jitter buffer over wall-clock arrival
		// times, and a paused or time-scaled game must not warp it.
		[[nodiscard]] float Now() const;

		// ── Script bridges ───────────────────────────────────────────────────
		// INJECTED, not constructed here. The concrete bridges are CoreCLR-backed
		// (CSharpScriptFieldBridge / CSharpRpcBridge), and building them in this TU
		// pulled the CLR host into everything that links it - which is why neither
		// this class nor the two network systems could be compiled into EngineTests
		// at all. The wiring site (Application.cpp, right after ScriptComponentSystem
		// is registered - which is also the ordering the old lazy resolve existed to
		// work around) constructs the real ones; a test passes a fake.
		//
		// Both stay null in a build with no CLR, and every consumer already
		// null-checks: the headless paths depend on that and must keep working.
		void SetFieldBridge(std::unique_ptr<ScriptFieldBridge> bridge);
		void SetRpcBridge(std::unique_ptr<RpcBridge> bridge);

		[[nodiscard]] const ScriptFieldBridge* FieldBridge() const;
		[[nodiscard]] const RpcBridge* Rpcs() const;

		// ── Framing ──────────────────────────────────────────────────────────
		// Every packet leads with one NetMessage byte so the receive system can
		// dispatch without a second framing layer. Spawn/Despawn/Rpc/Welcome already
		// carry theirs from their encoders; snapshots and script-field packets do
		// not, so they are wrapped here. The convention is documented, per kind, next
		// to the NetMessage enum in NetSpawn.hpp - Frame just forwards to
		// FrameMessage there.
		[[nodiscard]] static std::vector<std::byte> Frame(NetMessage kind, std::span<const std::byte> payload);
		[[nodiscard]] static std::vector<std::byte> EncodeWelcome(ConnectionId assigned);

		// ── Replicated entity lifecycle ──────────────────────────────────────
		// Host-only. Instantiates `prefab`, gives it a net id owned by `owner`, and
		// tells every connection to do the same. Returns an invalid entity when not
		// hosting or when the prefab cannot be read.
		Entity SpawnPrefab(World& world, const std::string& prefab, glm::vec3 position, ConnectionId owner);

		// Broadcasts the despawn (host only) and destroys the entity locally. A client
		// is refused outright for an entity it does not own: destroying it locally
		// while the host keeps replicating it desyncs this client permanently.
		void Despawn(World& world, Entity entity);

		// Clears every NetworkIdentity's net id. A session assigns scene-placed ids by
		// walking identities whose id is still 0, so ids left over from a previous
		// session would make the next one skip those entities entirely. Called for the
		// host in StartHost and for a client the moment the host's Welcome arrives.
		static void ResetForNewSession(World& world);

		// Client-side application of a replayed or live Spawn. Ignores a net id it
		// already knows (a scene-placed entity both ends already have) and a spawn
		// message naming no prefab, or one that could reach outside the prefab folder.
		void ApplySpawn(World& world, const SpawnMessage& msg);
		void ApplyDespawn(World& world, std::uint32_t netId);

		// Client-side reaction to NetMessage::Relevancy - this connection no longer
		// needs to care about `netId`, though it is still alive on the host. Behaves
		// exactly like ApplyDespawn today: the client has no representation for
		// "exists somewhere, just not here", only "have it" or "don't". Kept as a
		// separate entry point rather than a straight ApplyDespawn call from OnData
		// so a future difference - a network debug overlay distinguishing a real
		// destroy from relevancy churn, client-side pooling that recycles instead of
		// destroying - is a one-function change here instead of a wire-format
		// cutover across every deployed client.
		void ApplyRelevancyLeave(World& world, std::uint32_t netId);

		// Authority: the host decides everything; a client decides only what it
		// owns. Offline every entity is local, so both are true and single-player
		// code written against them just works.
		[[nodiscard]] bool HasAuthority(World& world, Entity entity) const;
		[[nodiscard]] bool IsOwner(World& world, Entity entity) const;

	private:
		ServiceContainer& m_services;
		NetworkSubsystem m_transport;
		NetSession m_session;
		ReplicationSchema m_schema;
		RelevancySettings m_relevancy;
		float m_sendRateHz = 20.f;
		std::unordered_map<ConnectionId, SnapshotCache> m_caches;

		std::chrono::steady_clock::time_point m_epoch = std::chrono::steady_clock::now();

		std::unique_ptr<ScriptFieldBridge> m_fieldBridge;
		std::unique_ptr<RpcBridge> m_rpcBridge;
	};
} // namespace aether::net
