#include "net/NetworkContext.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSerialize.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/SettingsService.hpp"

namespace aether::net
{
	namespace
	{
		// Default player cap for a listen server. Deliberately a local default and
		// not a tunable: a project that needs a different one passes it to StartHost.
		constexpr int kDefaultMaxConnections = 32;

		// Spare ENet peer slots ABOVE the player cap. A joiner has to be accepted at
		// the socket layer before anything can be said to it, so a host sized exactly
		// to its cap would have the socket layer silently refuse the (cap+1)th peer
		// and there would be no link left to send a reason down. The slack is what
		// turns "dropped" into "refused, and here is why".
		constexpr int kRefusalSlack = 4;

		// A body's type is baked into its b2BodyDef at creation and nothing re-reads
		// it afterwards, so changing RigidBody2DComponent::bodyType only takes effect
		// once the backing body is recreated. This is the same route the inspector and
		// MCP take for a body_type edit (RebuildBody2D in Physics2D.reflect.cpp), found
		// the same way - by name, through the world - so nothing about Physics2DSystem
		// has to know networking exists.
		//
		// A world with no physics system registered (a headless test, a 3D scene) is
		// not an error: setting the field is then the whole of the change, and the body
		// will be built with the right type if one is ever created.
		void RebuildBody2D(World& world, Entity entity)
		{
			if (auto* physics = static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem")))
			{
				physics->RebuildBody(world, entity);
			}
		}

		// Destroys everything ApplySpawn/SpawnPrefab instantiated for the session in this
		// world, and nothing else.
		//
		// Scene-placed entities stay - they came from the scene file and the next session
		// re-derives their ids from it - but a prefab-spawned one has no source but the
		// session that created it. Left behind, ResetForNewSession zeroes its netId,
		// AssignScenePlacedNetIds skips it (no node id), and the next join's replay
		// instantiates a second copy: every join/leave cycle would double the
		// prefab-spawned population.
		//
		// BOTH halves of the predicate are load-bearing. `scenePlaced` is only set by
		// AssignScenePlacedNetIds, so before the first session every scene-placed entity
		// still reads false - and Stop() also runs at the TOP of StartHost / StartClient.
		// Testing `scenePlaced` alone would therefore delete the entire replicated scene
		// the moment hosting began. A live net id is what marks an entity as belonging to
		// the session now ending.
		//
		// Collect first, destroy second: DestroyHierarchy mutates the registry the view
		// is iterating.
		void DestroySessionSpawned(World& world)
		{
			std::vector<Entity> spawned;
			world.View<NetworkIdentity>().each(
			        [&](entt::entity ent, NetworkIdentity& identity)
			        {
				        if (identity.netId != 0 && !identity.scenePlaced)
				        {
					        spawned.push_back(World::FromEntt(ent));
				        }
			        });
			for (const Entity entity: spawned)
			{
				ecs::DestroyHierarchy(world, entity);
			}
		}

		// Puts a handed-over body back on local simulation, at whatever type it was
		// authored as. Both routes back - this client gaining ownership, and the
		// session ending - go through here so the two can never disagree about what
		// "restored" means.
		void ReclaimBody(World& world, Entity entity)
		{
			const auto* marker = world.TryGet<NetSimulationOverride>(entity);
			if (marker == nullptr)
			{
				return;
			}
			const Body2DType authored = marker->authoredBodyType;
			world.Remove<NetSimulationOverride>(entity);
			if (auto* rigid = world.TryGet<RigidBody2DComponent>(entity))
			{
				rigid->bodyType = authored;
				RebuildBody2D(world, entity);
			}
		}

		// The 3D counterpart of RebuildBody2D. Jolt supports changing an existing
		// body's motion type live (BodyInterface::SetMotionType) - unlike Box2D there
		// is no destroy/rebuild step, but the "found by name through the world, so
		// PhysicsSystem never has to know networking exists" shape is the same, and
		// so is the "no physics system registered is not an error" fallback.
		void SetMotionType3D(World& world, Entity entity, PhysicsMotionType motionType)
		{
			if (auto* physics = static_cast<PhysicsSystem*>(world.FindSystem("PhysicsSystem")))
			{
				physics->SetBodyMotionType(world, entity, motionType);
			}
			else if (auto* rigid = world.TryGet<RigidBodyComponent>(entity))
			{
				rigid->motionType = motionType;
			}
		}

		// Seeds a body's real Jolt velocity from whatever the network last told this
		// peer about it (NetReceivedVelocity), applied only on reclaim - see that
		// call site's own comment for the bug this closes. Absent NetReceivedVelocity
		// (nothing has arrived yet, or this body was never non-owned in the first
		// place) is not an error: the body simply keeps whatever velocity it already
		// has, exactly as before this seam existed.
		void SeedVelocity3D(World& world, Entity entity, glm::vec3 linear, glm::vec3 angular)
		{
			auto* physics = static_cast<PhysicsSystem*>(world.FindSystem("PhysicsSystem"));
			const auto* rigid = world.TryGet<RigidBodyComponent>(entity);
			if (physics == nullptr || rigid == nullptr)
			{
				return;
			}
			physics->SetLinearVelocity(rigid->body, linear);
			physics->SetAngularVelocity(rigid->body, angular);
		}

		// The 3D counterpart of ReclaimBody. Called per-entity - once for a ragdoll's
		// root, and once more for each of its OTHER bones, each with its own marker -
		// see SyncSimulationAuthority's own comment for why a ragdoll needs every bone
		// frozen and thawed together rather than just its NetworkIdentity-carrying root.
		void ReclaimBody3D(World& world, Entity entity)
		{
			const auto* marker = world.TryGet<NetSimulationOverride3D>(entity);
			if (marker == nullptr)
			{
				return;
			}
			const PhysicsMotionType authored = marker->authoredMotionType;
			world.Remove<NetSimulationOverride3D>(entity);
			SetMotionType3D(world, entity, authored);

			// BUG THIS CLOSES: PushKinematicTargets (PhysicsSystem.cpp) drives a frozen
			// Kinematic body with BodyInterface::SetPositionAndRotation - a direct
			// teleport with no Jolt-side velocity implication, unlike Jolt's own
			// MoveKinematic, which would derive one from the position delta. So while
			// this body was non-owned, its real Jolt velocity sat wherever it was the
			// moment it froze (or wherever SetMotionType(...,Kinematic) itself reset it
			// to - empirically zero), NOT what NetVelocity's wire messages said it was
			// doing. Reclaiming to Dynamic with no further action would resume
			// simulating from that stale/zero velocity - a thrown prop that stops dead
			// in mid-air and drops the instant its owner changes. NetReceivedVelocity is
			// exactly the value ApplyVelocitySnapshot already wrote for relaying
			// purposes (see NetVelocity.hpp); reusing it here to seed the resumed body
			// costs nothing new on the wire.
			if (authored == PhysicsMotionType::Dynamic)
			{
				if (const auto* velocity = world.TryGet<NetReceivedVelocity>(entity))
				{
					SeedVelocity3D(world, entity, velocity->linear, velocity->angular);
				}
			}
		}

		// Ownership is per-RAGDOLL, not per-bone: only the root carries
		// NetworkIdentity (see RagdollBoneComponent's own comment), so a physics
		// gun that grabbed a LIMB has no netId of its own to claim, and without
		// this redirect RequestOwnershipTransfer's own "no NetworkIdentity ->
		// already mine" fallback would silently treat grabbing an arm as an
		// instant, meaningless local success - the entity would report "yours"
		// while SyncSimulationAuthority keeps it frozen Kinematic for everyone
		// but the ragdoll's ACTUAL owner. Redirecting to the root is the same
		// grouping SyncSimulationAuthority already freezes and thaws as a unit
		// (RagdollComponent::bones on the root) - claiming a limb claims the body
		// because the root's NetworkIdentity is the only ownership record that
		// exists for either. A non-bone entity (everything else) is returned
		// unchanged.
		Entity ResolveOwnershipEntity(World& world, Entity entity)
		{
			if (const auto* bone = world.TryGet<RagdollBoneComponent>(entity))
			{
				return bone->root;
			}
			return entity;
		}
	} // namespace

	NetworkContext::NetworkContext(ServiceContainer& services)
	      : m_services(services)
	      , m_schema(BuildReplicationSchema(reflect::ComponentTypes()))
	      , m_traversalSession(m_transport)
	{
	}

	// Out of line so the header needs only forward declarations of the two bridges.
	NetworkContext::~NetworkContext() = default;

	const std::vector<reflect::ComponentType>& NetworkContext::Catalog() const
	{
		return reflect::ComponentTypes();
	}

	float NetworkContext::Now() const
	{
		const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - m_epoch;
		return elapsed.count();
	}

	// Held for the life of the context rather than rebuilt per frame: the field
	// bridge caches a per-type replicated-property table (invalidated by
	// CSharpScriptingSubsystem's reload generation) that a per-frame temporary would
	// throw away every tick.
	void NetworkContext::SetFieldBridge(std::unique_ptr<ScriptFieldBridge> bridge)
	{
		m_fieldBridge = std::move(bridge);
	}

	void NetworkContext::SetRpcBridge(std::unique_ptr<RpcBridge> bridge)
	{
		m_rpcBridge = std::move(bridge);
	}

	const ScriptFieldBridge* NetworkContext::FieldBridge() const
	{
		return m_fieldBridge.get();
	}

	const RpcBridge* NetworkContext::Rpcs() const
	{
		return m_rpcBridge.get();
	}

	bool NetworkContext::StartHost(World& world, std::uint16_t port, int maxConnections)
	{
		Stop(world);
		m_disconnectReason.clear();
		m_maxConnections = maxConnections > 0 ? maxConnections : kDefaultMaxConnections;
		if (!m_transport.Host(port, m_maxConnections + kRefusalSlack))
		{
			AE_WARN(LogCategory::App, "Net: host failed - {}", m_transport.LastError());
			return false;
		}
		m_session.SetRole(NetRole::Host);
		m_session.SetLocalConnection(kInvalidConnection);
		// The host's own scene-placed entities get their ids now; every client that
		// loads the same scene derives the identical ids with no handshake.
		ResetForNewSession(world);
		AssignScenePlacedNetIds(world, m_session);
		return true;
	}

	bool NetworkContext::StartClient(World& world, std::string_view host, std::uint16_t port)
	{
		Stop(world);
		// Cleared HERE and nowhere else. Stop() is what runs when a link dies, so a
		// reason cleared there would be gone before the game could read it; clearing it
		// as a new attempt begins is what stops the previous session's reason being
		// mistaken for this one's.
		m_disconnectReason.clear();
		if (!m_transport.Connect(host, port))
		{
			AE_WARN(LogCategory::App, "Net: connect failed - {}", m_transport.LastError());
			return false;
		}
		// Role is Client immediately, but LocalConnection stays invalid until the
		// host's Welcome arrives - which is what IsConnected() reports.
		m_session.SetRole(NetRole::Client);
		m_session.SetLocalConnection(kInvalidConnection);
		return true;
	}

	void NetworkContext::Stop(World& world)
	{
		// FIRST, while the socket is still up: tell everybody this was deliberate.
		// Without it a client cannot tell a host that quit from a host that crashed or
		// a network that dropped, and those want opposite reactions - accept it and go
		// back to the menu, versus try to come back. Flushed explicitly because the
		// transport is destroyed a few lines below and there is no later service call
		// to push the queue out.
		//
		// Host-only, and only when somebody is listening: a client leaving says nothing
		// (its ENet disconnect is the whole message), and a host with no connections has
		// nobody to tell.
		if (IsHost() && !m_session.Connections().empty())
		{
			m_transport.Broadcast(kChannelReliable, true, EncodeDisconnect(kReasonHostClosed));
			m_transport.Flush();
		}

		// A body this client took off local simulation belongs
		// to the local simulation again the moment there is no session driving it.
		// Scene-placed entities survive Stop, so one left kinematic here is a player
		// character that never falls again in single-player. Stop also runs at the TOP
		// of StartHost/StartClient, which is what un-does a previous client session's
		// handovers when the same process goes on to host.
		RestoreSimulationAuthority(world);

		// Everything ApplySpawn/SpawnPrefab instantiated for this session goes with it -
		// see the note on DestroySessionSpawned for why `scenePlaced` decides.
		DestroySessionSpawned(world);

		m_transport.Disconnect();
		m_session.Clear();
		m_caches.clear();
		m_resyncRequests.clear();
		m_lastResyncAt.clear();
		m_traversalHostSetupPending = false;
	}

	void NetworkContext::ConfigureTraversalFromSettings()
	{
		// No SettingsService registered (a headless tool, or a test constructing this
		// directly) leaves m_traversalSession on its own defaults, which already mirror
		// EngineSettings::Network's - see NetTraversalSession.hpp.
		SettingsService* settings = m_services.TryGet<SettingsService>();
		if (settings == nullptr)
		{
			return;
		}
		const EngineSettings::Network& network = settings->Get().network;
		m_traversalSession.SetStunServer(network.stunHost, static_cast<std::uint16_t>(network.stunPort));
		m_traversalSession.SetTurnServer(network.turnHost, static_cast<std::uint16_t>(network.turnPort), network.turnUsername,
		        network.turnPassword, network.allowRelay);
		m_traversalSession.SetRendezvousDefault(network.rendezvousHost, static_cast<std::uint16_t>(network.rendezvousPort));
	}

	bool NetworkContext::HostWithCode(std::string_view roomCode, std::uint16_t port, int maxConnections)
	{
		// A traversal attempt binds the transport exactly like StartHost does, so an
		// already-active session - direct-IP, or a traversal already under way -
		// must go through Stop() first, which needs World and this call does not
		// have. Refusing outright is simpler and safer than tearing one down halfway.
		if (IsActive())
		{
			return false;
		}
		ConfigureTraversalFromSettings();
		m_disconnectReason.clear();
		m_maxConnections = maxConnections > 0 ? maxConnections : kDefaultMaxConnections;
		// The slack is the same reason StartHost adds it above - see kRefusalSlack -
		// and applies here too: m_maxConnections itself stays the pure player cap
		// IsFull() compares against.
		const bool started = m_traversalSession.HostWithCode(roomCode, port, m_maxConnections + kRefusalSlack);
		if (started)
		{
			m_session.SetRole(NetRole::Host);
			m_session.SetLocalConnection(kInvalidConnection);
			m_traversalHostSetupPending = true;
		}
		return started;
	}

	bool NetworkContext::JoinByCode(std::string_view roomCode)
	{
		if (IsActive())
		{
			return false;
		}
		ConfigureTraversalFromSettings();
		m_disconnectReason.clear();
		const bool started = m_traversalSession.JoinByCode(roomCode);
		if (started)
		{
			// Set now rather than left at Offline for however long the punch takes:
			// SpawnPrefab and NetRpc target resolution both read m_session.Role()
			// directly, and a role of Offline through that whole window would let
			// them behave as if this were single-player.
			m_session.SetRole(NetRole::Client);
			m_session.SetLocalConnection(kInvalidConnection);
		}
		return started;
	}

	void NetworkContext::TickTraversal(World& world, float dt)
	{
		m_traversalSession.Tick(dt);

		// Deferred from HostWithCode, which has no World: a host's scene-placed net
		// ids must exist before the first real joiner can arrive, and this always
		// runs at least a frame ahead of that - no real network round trip completes
		// within the same frame HostWithCode was called on.
		if (m_traversalHostSetupPending)
		{
			m_traversalHostSetupPending = false;
			ResetForNewSession(world);
			AssignScenePlacedNetIds(world, m_session);
		}
	}

	void NetworkContext::SetReplicationReady(World& world, bool ready)
	{
		if (m_replicationReady == ready)
		{
			return; // no transition, so nothing to announce and nothing to rebuild
		}
		m_replicationReady = ready;
		if (!ready)
		{
			// From here until it says otherwise this peer ignores spawns and state - see
			// the guard in NetworkReceiveSystem::OnData. Nothing is queued: a message
			// this client refuses is one the host will send again when asked.
			AE_INFO(LogCategory::App, "Net: holding replication - this peer is not in the session's scene");
			return;
		}
		if (!IsClient() || !IsConnected())
		{
			// Offline, hosting, or not welcomed yet. The flag is read on arrival
			// instead, so a client that becomes ready before its Welcome lands is
			// simply ready when the replay reaches it.
			return;
		}
		// ARRIVAL. Whatever the session left in this world belongs to the scene this
		// peer has left, so it goes; the scene-placed ids are re-derived against the
		// scene it is in NOW (the derivation ran when the Welcome landed, which was in
		// whatever scene the join was started from); and the host is told to send the
		// world again, because it has been replicating to a peer that was discarding it.
		DestroySessionSpawned(world);
		ResetForNewSession(world);
		m_session.ResetBindings();
		AssignScenePlacedNetIds(world, m_session);
		m_transport.Send(kInvalidConnection, kChannelReliable, true, EncodeClientReady());
		AE_INFO(LogCategory::App, "Net: ready for replication - asked the host for the world");
	}

	bool NetworkContext::ConsumeResyncRequest(ConnectionId connection)
	{
		return m_resyncRequests.erase(connection) != 0;
	}

	void NetworkContext::RefuseConnection(ConnectionId peer, std::string_view reason)
	{
		if (!IsHost())
		{
			return;
		}
		// Reliable, and DisconnectPeer defers the drop until everything already queued
		// for that peer has left the socket (enet_peer_disconnect_later). Dropping
		// first, or dropping now, would tear the link down on top of the very message
		// that explains it, and the joiner would see an anonymous failure - which is
		// what DisconnectReason exists to prevent: a refusal must be tellable apart
		// from a timeout or a crashed host, because a game retries one and not the
		// other.
		m_transport.Send(peer, kChannelReliable, true, EncodeDisconnect(reason));
		m_transport.DisconnectPeer(peer);
		// Never added to the session: a refused joiner was never a member, so there is
		// no binding, cache or roster entry to clean up here.
	}

	void NetworkContext::RequestResync(ConnectionId connection)
	{
		// The ROLE gate is not repeated here: it lives with every other one, at the top of
		// the matching case in NetworkReceiveSystem::OnData, which is this function's only
		// caller. Two copies of "only the host answers this" is two places for it to be
		// true, which is one more than a test can distinguish.
		if (connection == kInvalidConnection)
		{
			return; // never a client's id - it is how the transport spells "the host"
		}
		// A resync costs a full reliable state send plus a Spawn per newly relevant
		// entity, and ClientReady is one byte a peer may loop at line rate - the send
		// tick consumes a queued request every paced tick, so an unthrottled request
		// set pins this host into streaming the whole replicated world at SendRateHz
		// to that one peer. One honoured request per resync interval bounds the cost to
		// what the periodic full resend already spends, while the genuine handshake -
		// a client that has just arrived in the scene - sends exactly one and never
		// notices the floor.
		const float now = Now();
		if (const auto it = m_lastResyncAt.find(connection); it != m_lastResyncAt.end()
		        && now - it->second < kResyncIntervalSeconds)
		{
			return;
		}
		m_lastResyncAt[connection] = now;
		m_resyncRequests.insert(connection);
	}

	void NetworkContext::ForgetNetId(std::uint32_t netId)
	{
		for (auto& [connection, cache]: m_caches)
		{
			cache.Forget(netId);
		}
	}

	std::vector<std::byte> NetworkContext::Frame(NetMessage kind, std::span<const std::byte> payload)
	{
		return FrameMessage(kind, payload);
	}

	std::vector<std::byte> NetworkContext::EncodeWelcome(ConnectionId assigned)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Welcome));
		w.U32(assigned);
		return w.Take();
	}

	Entity NetworkContext::SpawnPrefab(World& world, const std::string& prefab, glm::vec3 position, ConnectionId owner)
	{
		// A client may not allocate a net id, so it may not spawn. Offline there is no
		// id to allocate and nobody to tell, and the entity is simply built here - see
		// the offline-parity note on the declaration.
		const bool offline = m_session.Role() == NetRole::Offline;
		if ((!IsHost() && !offline) || prefab.empty())
		{
			return {};
		}
		const auto description = aether::app::scene::ReadPrefabFile(prefab);
		if (!description.has_value())
		{
			AE_WARN(LogCategory::App, "Net: cannot spawn unknown prefab '{}'", prefab);
			return {};
		}

		const glm::mat4 xform = glm::translate(glm::mat4(1.0f), position);
		const Entity root = aether::app::scene::InstantiatePrefab(*description, world,
		        aether::app::scene::MakeApplySceneDeps(m_services), xform, nullptr, /*markTransient=*/true);
		if (!root.IsValid())
		{
			return {};
		}

		if (offline)
		{
			// No identity is stamped and nothing is bound. The prefab's own
			// NetworkIdentity (if it authored one) stays at net id 0, which is what
			// every offline entity looks like, and OwnsIdentity reports this process
			// as the owner of everything while the role is Offline - so HasAuthority
			// and IsOwner both answer true for it, exactly as the API promises.
			return root;
		}

		NetworkIdentity identity{};
		identity.netId = m_session.AllocateNetId();
		if (identity.netId == 0)
		{
			// The spawn id space is exhausted (~4.3 billion allocations in one
			// session). The entity stays local and unreplicated - the same shape
			// AssignScenePlacedNetIds leaves on scene-space exhaustion - rather than
			// binding net id 0, which every decoder treats as "no id", or
			// broadcasting a spawn no receiver can apply.
			AE_ERROR(LogCategory::App, "Net: spawn id space exhausted - '{}' stays local to this peer", prefab);
			return root;
		}
		identity.owner = owner;
		identity.spawnPrefab = prefab;
		identity.scenePlaced = false;
		world.EmplaceOrReplace<NetworkIdentity>(root, identity);
		m_session.Bind(identity.netId, root);

		const std::vector<std::byte> packet = EncodeSpawn(identity.netId, owner, prefab, position);
		m_transport.Broadcast(kChannelReliable, true, packet);
		return root;
	}

	bool NetworkContext::ReleaseForDespawn(World& world, Entity entity)
	{
		if (!entity.IsValid())
		{
			return false;
		}
		// A client destroying something it does not own desyncs only itself: the
		// broadcast below is host-only, so the host keeps replicating a net id that now
		// resolves to nothing on this machine and the entity never comes back. Refuse
		// instead. IsOwner is true for an entity with no NetworkIdentity, so a client's
		// purely local entities are still destroyable through Net.Despawn.
		if (IsClient() && !IsOwner(world, entity))
		{
			return false;
		}
		// A scene-placed entity is not this session's to destroy, on either role. The
		// broadcast would delete it on today's clients while the NEXT joiner re-derives
		// it from the scene file - permanent divergence (the exact hazard every other
		// scenePlaced-sensitive path - Stop, OnDisconnected, ClientCanRecreate - already
		// refuses); and a client destroying only its own copy strands a frozen entity on
		// every peer, since ExceptOwnedBy stops the host relaying state the owner no
		// longer sends and re-entry cannot rebuild something with no prefab. Refusing is
		// convergent: every peer keeps it, and a game that wants it gone despawns a
		// framework-spawned replacement instead. Offline the refusal is lifted - there
		// is no session to diverge, and single-player Despawn must keep working.
		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity != nullptr && identity->scenePlaced && m_session.Role() != NetRole::Offline)
		{
			AE_WARN(LogCategory::App,
			        "Net: refusing to despawn scene-placed entity {} - its existence belongs to the scene file, not the session; despawn a framework-spawned entity instead",
			        m_session.NetIdFor(entity));
			return false;
		}
		const std::uint32_t netId = m_session.NetIdFor(entity);
		if (IsHost() && netId != 0)
		{
			const std::vector<std::byte> packet = EncodeDespawn(netId);
			m_transport.Broadcast(kChannelReliable, true, packet);
		}
		if (netId != 0)
		{
			m_session.Unbind(netId);
			ForgetNetId(netId);
		}
		return true;
	}

	void NetworkContext::Despawn(World& world, Entity entity)
	{
		if (!ReleaseForDespawn(world, entity))
		{
			return;
		}
		// A prefab instantiates as a hierarchy; destroying only the root would strand
		// every child in the world with no owner and no way to reach them.
		ecs::DestroyHierarchy(world, entity);
	}

	void NetworkContext::ApplySpawn(World& world, const SpawnMessage& msg)
	{
		if (const Entity existing = m_session.EntityFor(msg.netId); existing.IsValid())
		{
			// Already bound. The common case is the joining-client replay of a
			// scene-placed entity: both ends derived the same id from the scene file,
			// so there is nothing to create.
			if (msg.prefab.empty())
			{
				return;
			}
			auto* identity = world.TryGet<NetworkIdentity>(existing);
			if (identity == nullptr || !identity->scenePlaced)
			{
				// A Spawn for an id bound to an already-spawned entity is the join
				// replay re-sending something this client already has - a no-op.
				return;
			}
			// Hard desync, unreachable since the id spaces were split (scene ids
			// derive below kSpawnNetIdBase, spawned ids allocate above it) but kept
			// as the safety net: it fires only if a peer violates that split - a
			// hostile or version-skewed Spawn naming a prefab for an id this client
			// derived for a scene-placed entity. The host owns id allocation, so the
			// binding moves to its spawn instead of being silently dropped; the
			// scene entity stays locally, unbound.
			AE_ERROR(LogCategory::App,
			        "Net: net id {} collision - the host spawned a prefab onto an id this client derived for a scene-placed entity; rebinding to the host's spawn",
			        msg.netId);
			identity->netId = 0;
			m_session.Unbind(msg.netId);
			ForgetNetId(msg.netId); // interpolation cache must not carry the scene entity onto the spawn
		}

		if (!IsSafePrefabName(msg.prefab))
		{
			// Either a scene-placed entity this client does not have - nothing generic
			// can recreate it, since the framework does not know how the project loads
			// scenes - or a name a peer built to reach outside the prefab directory.
			return;
		}
		const auto description = aether::app::scene::ReadPrefabFile(msg.prefab);
		if (!description.has_value())
		{
			AE_WARN(LogCategory::App, "Net: spawn names prefab '{}', which this build has no asset for", msg.prefab);
			return;
		}

		const glm::mat4 xform = glm::translate(glm::mat4(1.0f), msg.position);
		const Entity root = aether::app::scene::InstantiatePrefab(*description, world,
		        aether::app::scene::MakeApplySceneDeps(m_services), xform, nullptr, /*markTransient=*/true);
		if (!root.IsValid())
		{
			return;
		}

		NetworkIdentity identity{};
		identity.netId = msg.netId;
		identity.owner = msg.owner;
		identity.spawnPrefab = msg.prefab;
		identity.scenePlaced = false;
		world.EmplaceOrReplace<NetworkIdentity>(root, identity);
		m_session.Bind(identity.netId, root);
	}

	void NetworkContext::ResetForNewSession(World& world)
	{
		world.View<NetworkIdentity>().each([](NetworkIdentity& identity) { identity.netId = 0; });
	}

	void NetworkContext::ApplyDespawn(World& world, std::uint32_t netId)
	{
		const Entity entity = m_session.EntityFor(netId);
		m_session.Unbind(netId);
		ForgetNetId(netId);
		if (entity.IsValid())
		{
			ecs::DestroyHierarchy(world, entity);
		}
	}

	void NetworkContext::ApplyRelevancyLeave(World& world, std::uint32_t netId)
	{
		ApplyDespawn(world, netId);
	}

	OwnershipTransferOutcome NetworkContext::RequestOwnershipTransfer(World& world, Entity entity, ConnectionId newOwner)
	{
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return OwnershipTransferOutcome::Refused;
		}
		// A grabbed ragdoll LIMB redirects to its root - see ResolveOwnershipEntity's
		// own comment for why "claiming a limb claims the body" has to happen here.
		entity = ResolveOwnershipEntity(world, entity);
		auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity == nullptr || identity->netId == 0)
		{
			// Not a replicated entity, or not yet part of a session (a client before
			// its Welcome derives no ids at all) - there is no owner field to change,
			// so this process already IS the owner, exactly as IsOwner's own nullptr
			// branch already answers. Nothing to send, nothing to apply.
			return OwnershipTransferOutcome::Applied;
		}

		if (m_session.Role() == NetRole::Offline)
		{
			identity->owner = newOwner;
			return OwnershipTransferOutcome::Applied;
		}

		if (IsHost())
		{
			// The host's own decision needs no validation against
			// ValidateOwnershipRequest - that gate exists for an inbound CLIENT
			// request; the host is this session's authority for every other mutation
			// (Spawn, Despawn, a disconnect's release reason) and ownership is no
			// different.
			identity->owner = newOwner;
			m_transport.Broadcast(kChannelReliable, true, EncodeOwnershipTransfer(identity->netId, newOwner));
			return OwnershipTransferOutcome::Applied;
		}

		// Client: ask, and wait. `identity->owner` changes only once the host's
		// broadcast lands - see ApplyOwnershipTransfer.
		m_transport.Send(kInvalidConnection, kChannelReliable, true,
		        EncodeOwnershipRequest(identity->netId, newOwner));
		return OwnershipTransferOutcome::Requested;
	}

	void NetworkContext::ApplyOwnershipTransfer(World& world, std::uint32_t netId, ConnectionId newOwner)
	{
		const Entity entity = m_session.EntityFor(netId);
		if (auto* identity = entity.IsValid() ? world.TryGet<NetworkIdentity>(entity) : nullptr)
		{
			identity->owner = newOwner;
		}
	}

	// Deliberately IsOwner verbatim, not "the host decides everything" - see the note
	// on the declaration. Under client authority the two questions have one answer.
	bool NetworkContext::HasAuthority(World& world, Entity entity) const
	{
		return IsOwner(world, entity);
	}

	bool NetworkContext::IsOwner(World& world, Entity entity) const
	{
		// A stale handle - kept around by a script or a cached reference after the
		// entity behind it died - carries no components at all: entt strips them on
		// destroy(). That made the identity == nullptr branch below misread it as
		// "not replicated, so it's mine", which is the right answer for a genuinely
		// local entity and the wrong one for a dead replicated one. Reject it before
		// the identity lookup even runs, so a destroyed handle can never read as
		// owned - and Entity carries no version (see Entity.hpp), so `valid()` here
		// really is the only test available; it also catches the trivial case of an
		// id that was never created at all.
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return false;
		}
		entity = ResolveOwnershipEntity(world, entity);
		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity == nullptr)
		{
			// Not a replicated entity. Offline and on the host that is simply "mine";
			// on a client an unreplicated entity is local-only, so it is mine too.
			return true;
		}
		return OwnsIdentity(*identity);
	}

	bool NetworkContext::OwnsIdentity(const NetworkIdentity& identity) const
	{
		if (m_session.Role() == NetRole::Offline)
		{
			// No session: this process is the whole world, so it owns every entity in
			// it whatever a NetworkIdentity happens to say. The owner id is not
			// cleared when a session ends (only the net id is), so without this an
			// entity left over from a session - or one authored with an owner - would
			// read as somebody else's in single-player and stop being simulated. The
			// documented contract is "offline, both report true"; this is what makes
			// that unconditional rather than true-by-coincidence.
			return true;
		}
		// A client has no connection id of its own until the host's Welcome
		// arrives, and LocalConnection() reads kInvalidConnection until then - the
		// same value every host-owned entity's owner defaults to. Left unguarded,
		// a client briefly "owns" the entire world between Connect() and Welcome.
		// A host's own LocalConnection is kInvalidConnection permanently (that is
		// how it owns its own entities), so the guard only applies to a client.
		if (IsClient() && m_session.LocalConnection() == kInvalidConnection)
		{
			return false;
		}
		return identity.owner == m_session.LocalConnection();
	}

	std::string NetworkContext::ResolveDisplayName(World& world, Entity self, std::string_view desired) const
	{
		std::string wanted = SanitizeReason(desired);
		// Trim, so " " and "" cannot become two different "blank" names.
		const auto notSpace = [](unsigned char c) { return c != ' '; };
		wanted.erase(wanted.begin(), std::find_if(wanted.begin(), wanted.end(), notSpace));
		wanted.erase(std::find_if(wanted.rbegin(), wanted.rend(), notSpace).base(), wanted.end());
		if (wanted.empty())
		{
			wanted = "Player";
		}

		// Where `self` sits in the join order. The host is connection 0, so it is always
		// first and never renamed; the entity id breaks ties between two players one
		// connection happens to own, which keeps the order total.
		const auto* selfIdentity = world.TryGet<NetworkIdentity>(self);
		const ConnectionId selfOwner = selfIdentity != nullptr ? selfIdentity->owner : m_session.LocalConnection();
		const std::pair<ConnectionId, std::uint32_t> selfRank{selfOwner, self.id};

		std::unordered_set<std::string> taken;
		world.View<NetPlayer>().each(
		        [&](entt::entity ent, NetPlayer& player)
		        {
			        const Entity entity = World::FromEntt(ent);
			        if (entity.id == self.id || player.displayName.empty())
			        {
				        return;
			        }
			        const auto* identity = world.TryGet<NetworkIdentity>(entity);
			        const ConnectionId owner = identity != nullptr ? identity->owner : kInvalidConnection;
			        // ONLY players ahead of us in the order. A player behind us will step
			        // aside itself, and treating it as an obstacle here is exactly how two
			        // peers picking the same name at the same moment would both move and
			        // collide again on the next frame.
			        if (std::pair<ConnectionId, std::uint32_t>{owner, entity.id} < selfRank)
			        {
				        taken.insert(player.displayName);
			        }
		        });

		if (!taken.contains(wanted))
		{
			return wanted;
		}
		// " (2)", " (3)", ... Bounded by the number of obstacles plus two, so it always
		// terminates on a free name however contrived the set is.
		for (std::size_t suffix = 2; suffix <= taken.size() + 2; ++suffix)
		{
			std::string candidate = wanted + " (" + std::to_string(suffix) + ")";
			if (!taken.contains(candidate))
			{
				return candidate;
			}
		}
		return wanted;
	}

	std::string NetworkContext::ClaimPlayerName(World& world, Entity self, std::string_view desired)
	{
		// THE OWNER AUTHORS ITS OWN NAME. A display name is replicated state, so the
		// same rule that governs a transform governs it: a peer writing it on somebody
		// else's entity is writing a value that entity's owner will overwrite on its
		// next send, and whether the write survives is a race.
		if (!IsOwner(world, self))
		{
			return {};
		}
		std::string resolved = ResolveDisplayName(world, self, desired);
		if (auto* player = world.TryGet<NetPlayer>(self))
		{
			player->displayName = resolved;
		}
		else
		{
			world.Emplace<NetPlayer>(self, NetPlayer{.displayName = resolved});
		}
		return resolved;
	}

	void NetworkContext::SyncSimulationAuthority(World& world)
	{
		// An offline game has no other authority to defer to, so it never hands a body
		// over. Nothing happens on a client before the Welcome either: until
		// LocalConnection is real, OwnsIdentity answers false for everything (see its
		// guard), and acting on that would hand over this client's OWN character for
		// the length of the handshake and then rebuild its body a second time to give
		// it back. IsConnected() is true for a host the moment it is listening and for
		// a client only once welcomed, which is exactly that rule.
		if (!IsConnected())
		{
			return;
		}

		// Collected first, applied second: both branches below add or remove a
		// component, and RebuildBody2D strips Physics2DStateComponent - all of which
		// invalidate the view being walked.
		std::vector<Entity> handover;
		std::vector<Entity> reclaim;
		world.View<NetworkIdentity, RigidBody2DComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, RigidBody2DComponent& rigid)
		        {
			        if (identity.netId == 0)
			        {
				        return; // not part of this session yet; nothing is replicating it
			        }
			        const Entity entity = World::FromEntt(ent);
			        const bool handedOver = world.Has<NetSimulationOverride>(entity);
			        if (OwnsIdentity(identity))
			        {
				        // Ours to simulate, and the ONLY entity on this machine that is:
				        // scripts drive it, physics integrates it, and the result is what
				        // this peer puts on the wire.
				        if (handedOver)
				        {
					        reclaim.push_back(entity);
				        }
				        return;
			        }
			        // These two together are what make the reconcile idempotent, and
			        // this runs EVERY frame, so an unconditional handover here would
			        // destroy and rebuild the Box2D body of every remote entity on every
			        // tick - and, worse, re-record the CURRENT body type as the authored
			        // one, so the type to restore would decay to Kinematic after a single
			        // frame and nothing could ever be given back.
			        if (handedOver)
			        {
				        return;
			        }
			        // Only a Dynamic body is a problem: Static and Kinematic ones are
			        // already not integrated, and SyncTransforms skips both, so the
			        // replicated transform already survives the frame untouched.
			        if (rigid.bodyType != Body2DType::Dynamic)
			        {
				        return;
			        }
			        handover.push_back(entity);
		        });

		for (const Entity entity: handover)
		{
			auto* rigid = world.TryGet<RigidBody2DComponent>(entity);
			if (rigid == nullptr)
			{
				continue;
			}
			world.EmplaceOrReplace<NetSimulationOverride>(entity,
			        NetSimulationOverride{.authoredBodyType = rigid->bodyType});
			rigid->bodyType = Body2DType::Kinematic;
			RebuildBody2D(world, entity);
		}
		for (const Entity entity: reclaim)
		{
			ReclaimBody(world, entity);
		}

		// ── 3D ────────────────────────────────────────────────────────────────
		// Same shape as the 2D walk above (see its own comments for the reasoning
		// behind every branch) - a non-owned Dynamic RigidBodyComponent becomes
		// Kinematic via PhysicsSystem::SetBodyMotionType, and PhysicsSystem's own
		// PushKinematicTargets/SyncTransforms give a Kinematic body exactly the same
		// transform-driven semantics Physics2DSystem already has.
		std::vector<Entity> handover3D;
		std::vector<Entity> reclaim3D;
		world.View<NetworkIdentity, RigidBodyComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, RigidBodyComponent& rigid)
		        {
			        if (identity.netId == 0)
			        {
				        return;
			        }
			        const Entity entity = World::FromEntt(ent);
			        const bool handedOver = world.Has<NetSimulationOverride3D>(entity);
			        if (OwnsIdentity(identity))
			        {
				        if (handedOver)
				        {
					        reclaim3D.push_back(entity);
				        }
				        return;
			        }
			        if (handedOver)
			        {
				        return;
			        }
			        if (rigid.motionType != PhysicsMotionType::Dynamic)
			        {
				        return;
			        }
			        handover3D.push_back(entity);
		        });

		// A ragdoll is many jointed bodies, and only its root carries NetworkIdentity
		// - the view above can only ever find that one. Freezing the root alone would
		// leave its OTHER bones fully Dynamic and jointed to a body that now teleports
		// to wherever the network says every tick: a Hinge/Swing Twist constraint
		// between a teleporting Kinematic body and a live Dynamic one is exactly the
		// mixed-authority case a constraint solver was never asked to make sense of,
		// and the failure mode is jitter or an outright explosion, not a stale pose.
		// So a ragdoll is frozen and thawed AS A UNIT: every bone in
		// RagdollComponent::bones gets its own NetSimulationOverride3D and its own
		// Kinematic/Dynamic flip, driven from the root's ownership. This does not by
		// itself give a non-owner an accurate view of a ragdoll's POSE - only the
		// root has a NetworkTransform to receive one - it only guarantees the frozen
		// state is safe rather than fighting itself; full per-bone ragdoll
		// replication is a separate, larger feature this does not attempt.
		for (const Entity entity: handover3D)
		{
			auto* rigid = world.TryGet<RigidBodyComponent>(entity);
			if (rigid == nullptr)
			{
				continue;
			}
			world.EmplaceOrReplace<NetSimulationOverride3D>(entity, NetSimulationOverride3D{.authoredMotionType = rigid->motionType});
			SetMotionType3D(world, entity, PhysicsMotionType::Kinematic);
			if (const auto* ragdoll = world.TryGet<RagdollComponent>(entity))
			{
				for (const Entity bone: ragdoll->bones)
				{
					if (bone == entity)
					{
						continue;
					}
					auto* boneRigid = world.TryGet<RigidBodyComponent>(bone);
					if (boneRigid == nullptr || boneRigid->motionType != PhysicsMotionType::Dynamic)
					{
						continue;
					}
					world.EmplaceOrReplace<NetSimulationOverride3D>(bone, NetSimulationOverride3D{.authoredMotionType = boneRigid->motionType});
					SetMotionType3D(world, bone, PhysicsMotionType::Kinematic);
				}
			}
		}
		for (const Entity entity: reclaim3D)
		{
			if (const auto* ragdoll = world.TryGet<RagdollComponent>(entity))
			{
				for (const Entity bone: ragdoll->bones)
				{
					if (bone != entity)
					{
						ReclaimBody3D(world, bone);
					}
				}
			}
			ReclaimBody3D(world, entity);
		}

		// ── Character Controller ────────────────────────────────────────────────
		// No marker component needed: locallySimulated is a plain, harmless-to-set
		// -every-frame boolean (see its own declaration) - StepCharacters already
		// treats false as "mirror TransformComponent instead of integrating", so
		// flipping it is the whole change, unlike the Rigid Body path above which
		// has to remember an authored type to give back.
		world.View<NetworkIdentity, CharacterControllerComponent>().each(
		        [&](entt::entity, NetworkIdentity& identity, CharacterControllerComponent& cc)
		        {
			        if (identity.netId == 0)
			        {
				        return;
			        }
			        cc.locallySimulated = OwnsIdentity(identity);
		        });
	}

	void NetworkContext::RestoreSimulationAuthority(World& world)
	{
		std::vector<Entity> handedOver;
		world.View<NetSimulationOverride>().each(
		        [&](entt::entity ent, NetSimulationOverride&) { handedOver.push_back(World::FromEntt(ent)); });
		for (const Entity entity: handedOver)
		{
			ReclaimBody(world, entity);
		}

		// 3D: every marked entity - a ragdoll's root AND each of its other bones
		// alike, since SyncSimulationAuthority gives each its own marker - is
		// reclaimed individually. No ragdoll-aware grouping needed here: each
		// marker already carries its own entity's own authored type.
		std::vector<Entity> handedOver3D;
		world.View<NetSimulationOverride3D>().each(
		        [&](entt::entity ent, NetSimulationOverride3D&) { handedOver3D.push_back(World::FromEntt(ent)); });
		for (const Entity entity: handedOver3D)
		{
			ReclaimBody3D(world, entity);
		}

		// Every character controller goes back to simulating itself locally -
		// offline, there is no other authority to defer to.
		world.View<CharacterControllerComponent>().each(
		        [](CharacterControllerComponent& cc) { cc.locallySimulated = true; });
	}
} // namespace aether::net
