#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetComponents.hpp"
#include "net/NetOwnership.hpp"
#include "net/NetRelevancy.hpp"
#include "net/NetSession.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetTraversalSession.hpp"
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

	// The two reasons the framework itself ends a link for. A game is free to send its
	// own wording through RefuseConnection; these exist so the common cases read the
	// same in every project and a test can assert on them verbatim.
	inline constexpr std::string_view kReasonServerFull = "Server is full";
	inline constexpr std::string_view kReasonHostClosed = "Host closed the session";

	// How long a peer may hold a stale value for an entity that has stopped
	// changing, when the diff carrying its last update was dropped: long enough that
	// the extra full state send is negligible beside the per-tick diffs, short enough
	// that a desync is not something a player experiences. Bounds BOTH directions -
	// the host's per-connection periodic resend and a client's upload of what it owns
	// (see NetworkSendSystem) - and is also the floor on how often a connection may
	// ASK for a resync, so a ClientReady looped at line rate cannot pin the host into
	// replaying the whole world every send tick (see RequestResync).
	inline constexpr float kResyncIntervalSeconds = 1.0f;

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
		//
		// `maxConnections` is a CAP ON PLAYERS, not just an ENet peer budget: the
		// (maxConnections + 1)th joiner is refused with a reason it can show rather
		// than being dropped by the socket layer with nothing to say. The ENet host is
		// deliberately created with room to spare (see kRefusalSlack) precisely so a
		// refused joiner can be accepted long enough to be told why. A non-positive
		// value means the framework default.
		bool StartHost(World& world, std::uint16_t port, int maxConnections);
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

		// A JOIN attempt also binds via NetworkSubsystem::Host() while it punches
		// (see NetTraversalSession's class comment), which makes the transport
		// report NetRole::Host for a peer that is not hosting anything and has
		// connected to nobody yet. Do not believe it until the punch either
		// finishes or gives up - JoinInProgress() names exactly that window.
		[[nodiscard]] bool IsHost() const
		{
			return m_transport.Role() == NetRole::Host && !m_traversalSession.JoinInProgress();
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

		// ── Link quality ─────────────────────────────────────────────────────
		// Round-trip time in milliseconds over the link named by `connection`, taken
		// from the transport's own acknowledgement timing - no probe packet exists and
		// none is needed.
		//
		// A CLIENT has one link and passes kInvalidConnection for it, which is how the
		// transport already spells "the host". A HOST names the connection it wants.
		// Offline, and for the host's own connection, the answer is 0: there is no link,
		// and 0 ms is the honest reading of "this peer, right here".
		[[nodiscard]] std::uint32_t RoundTripMs(ConnectionId connection) const
		{
			if (!IsActive() || connection == LocalConnectionId())
			{
				return 0;
			}
			return m_transport.RoundTripMs(connection);
		}

		// What this peer would report about ITSELF: a client's cost to reach the host,
		// and 0 on a host or offline. This is the value the owner stamps onto its own
		// player, which is what gives every peer a per-player ping with no extra traffic.
		[[nodiscard]] std::uint32_t LocalRoundTripMs() const
		{
			return IsClient() ? m_transport.RoundTripMs(kInvalidConnection) : 0u;
		}

		// ── Connection cap ───────────────────────────────────────────────────
		// How many simultaneous client connections this host accepts. The host itself
		// is not one of them, so a four-player game hosts with three.
		//
		// The cap lives here rather than in the game because every project needs one
		// and every project would otherwise reinvent the refusal handshake. WHAT the
		// number is, and how a refusal is worded to the player, stay the project's.
		[[nodiscard]] int MaxConnections() const
		{
			return m_maxConnections;
		}

		// True when one more connection would exceed the cap. Asked before a joiner is
		// admitted, so it counts the connections already in the session.
		[[nodiscard]] bool IsFull() const
		{
			return static_cast<int>(m_session.Connections().size()) >= m_maxConnections;
		}

		// Host-only: tell `peer` why it is not welcome, then drop it once that has
		// actually been sent. Never adds it to the session.
		void RefuseConnection(ConnectionId peer, std::string_view reason);

		// ── Why the last link ended ──────────────────────────────────────────
		// Non-empty only when the far end ended the link DELIBERATELY and said why: a
		// refusal, or a host closing the session. A link that dropped for any other
		// reason - a timeout, a pulled cable, a crashed host - leaves this empty, and
		// that difference is the whole point: it is what lets a game reconnect after an
		// accident without reconnecting into a session that just threw it out.
		//
		// Survives Stop() on purpose. Stop is what runs when the link dies, so a reason
		// cleared there would never be readable by the game. It is cleared when a NEW
		// session is started instead.
		[[nodiscard]] const std::string& DisconnectReason() const
		{
			return m_disconnectReason;
		}

		void SetDisconnectReason(std::string reason)
		{
			m_disconnectReason = std::move(reason);
		}

		// ── NAT traversal ────────────────────────────────────────────────────
		// Room-code hosting and joining: the ladder that asks the router for a
		// mapping, falls back to a rendezvous-and-punch over whichever
		// SignalingBackend is configured, and finishes by connecting through
		// whatever path opened - see NetTraversalSession, which does the actual
		// work this forwards to.
		//
		// Neither HostWithCode nor JoinByCode touches World: the bookkeeping that
		// does (assigning a host's scene-placed net ids) runs from TickTraversal
		// below instead, once a real frame has actually passed - see its comment
		// for why that is still soon enough.
		void ConfigureSignaling(SignalingBackend backend, std::string address)
		{
			m_traversalSession.ConfigureSignaling(backend, std::move(address));
		}

		bool HostWithCode(std::string_view roomCode, std::uint16_t port, int maxConnections);
		bool JoinByCode(std::string_view roomCode);

		[[nodiscard]] TraversalState GetTraversalState() const
		{
			return m_traversalSession.GetState();
		}

		[[nodiscard]] const std::string& TraversalFailureReason() const
		{
			return m_traversalSession.FailureReason();
		}

		// Drives the traversal ladder and finishes the host-side session setup the
		// moment it can safely run. Called once per frame from
		// NetworkReceiveSystem, right after Poll() - the one call in the per-frame
		// loop that already has World. A no-op whenever no attempt is running, so
		// every offline frame and every direct-IP session costs one state check.
		void TickTraversal(World& world, float dt);

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

		// Snapshots per link per second. State replication is rate-limited rather
		// than frame-locked: at 144 fps a frame-locked peer would spend most of its
		// upstream retransmitting sub-millimetre motion no receiver can show. Both
		// directions use it - the host's per-connection cadence and a client's upload
		// of what it owns.
		//
		// This is the OTHER half of the interpolation-delay budget
		// (InterpolationBuffer::RecommendedDelaySeconds): the auto-derived delay
		// settles toward roughly one send interval plus a jitter margin, so raising
		// this lowers how fresh remote motion can ever render, floor included.
		// Raising it is not free, which is why it stays a per-project call rather
		// than a bigger built-in default: bandwidth scales linearly with it (every
		// changed field, times every relevant entity, times every connection, times
		// this many times a second) and so does the per-connection, per-entity
		// change-detection scan NetworkSendSystem runs to decide what changed.
		// A twitch game that wants tighter default latency should raise this
		// explicitly and account for both costs, not get them silently.
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
		//
		// A client has exactly one link, and addresses it as kInvalidConnection -
		// which is how the transport already spells "the host" everywhere else.
		[[nodiscard]] SnapshotCache& CacheFor(ConnectionId connection)
		{
			return m_caches[connection];
		}

		// Everything this context holds FOR one connection, dropped together when that
		// connection goes. Keeping the two erases in one place is what stops a
		// disconnect leaving a resync request behind for an id the transport is free to
		// hand to the next joiner after a Stop/StartHost cycle.
		void DropCacheFor(ConnectionId connection)
		{
			m_caches.erase(connection);
			m_resyncRequests.erase(connection);
			m_lastResyncAt.erase(connection);
		}

		// Net ids are never reused, so a stale entry is only wasted memory - but a
		// long session that spawns and despawns constantly would grow every cache
		// without bound, so a despawn drops the entity from all of them.
		void ForgetNetId(std::uint32_t netId);

		// ── Is this peer standing in the session's scene? ────────────────────────
		// TRUE BY DEFAULT, and a game that never touches it behaves exactly as it
		// always has: the join replay lands on connect, nothing extra crosses the wire,
		// and no message is added to the handshake.
		//
		// It exists because a client applies a Spawn into whatever scene it happens to
		// be in, and a well-built game shows "connecting..." on its MENU. The host
		// answers a join with a Welcome and a Spawn for every relevant entity in one
		// burst, so a client still on the menu builds every player into the menu scene
		// and destroys them all a frame later on the scene change - permanently, because
		// the host remembers per connection what it has already sent and never offers
		// them again. The client ends up in the gameplay scene alone while the host
		// believes it spawned everybody.
		//
		// WHO DECIDES is the game, and deliberately so. The framework has no notion of
		// scene identity - it does not know how a project loads scenes, which is the
		// same reason a scene-placed entity has no spawn prefab - so the peer that knows
		// whether it is standing in the right world is the only one that can say. The
		// alternative, putting scene names on the wire, would make every project's scene
		// naming part of the protocol to answer a question one bool answers.
		//
		// Declaring FALSE (before connecting, on the menu) makes this client ignore
		// every inbound Spawn and every inbound state packet: none of it describes
		// anything this peer is holding. Declaring TRUE again is the arrival: whatever
		// the session left in this world is discarded, the scene-placed ids are
		// re-derived against the scene this peer is in NOW, and the host is asked - once,
		// with NetMessage::ClientReady - to send the world again from scratch.
		//
		// NOT reset by Stop(), and that is not an oversight: it is a statement about how
		// this game joins, not state belonging to one session, and StartClient() runs
		// Stop() internally - so a flag cleared there would be cleared out from under the
		// menu that set it a line earlier.
		void SetReplicationReady(World& world, bool ready);

		[[nodiscard]] bool IsReplicationReady() const
		{
			return m_replicationReady;
		}

		// Host side of the same handshake. A connection that has just said it is in the
		// session's scene is holding NOTHING, whatever this host previously sent it, so
		// the next send tick for it must be a full state send rather than a diff.
		//
		// Expressed as a request consumed by NetworkSendSystem rather than as a replay
		// issued here, because the send system already knows how to admit an entity to a
		// connection that has none of them: forget what that connection was believed to
		// hold and its relevancy transition does the whole job - a Spawn each for the
		// entities a client can rebuild, and one RELIABLE full snapshot behind them. A
		// second replay path would be a second thing to keep in step with relevancy,
		// with ClientCanRecreate, and with the reliable-resync rule.
		void RequestResync(ConnectionId connection);

		// True at most once per request. Consumed on the tick that acts on it, so a
		// request made while the connection is between paced sends is not lost.
		[[nodiscard]] bool ConsumeResyncRequest(ConnectionId connection);

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
		// Instantiates `prefab`, gives it a net id owned by `owner`, and tells every
		// connection to do the same. Returns an invalid entity when the prefab cannot
		// be read, and on a CLIENT, which may not allocate a net id.
		//
		// OFFLINE IT INSTANTIATES LOCALLY, which is the same offline parity Despawn
		// below has always had, and the rule the whole Net API is written to: a game
		// written for multiplayer must run unchanged with no session. Without it the
		// spawn half of a spawn/despawn pair silently did nothing in single-player
		// while the despawn half kept working, so a project that spawns anything
		// through the framework had a single-player mode with the spawns missing.
		Entity SpawnPrefab(World& world, const std::string& prefab, glm::vec3 position, ConnectionId owner);

		// Broadcasts the despawn (host only) and destroys the entity locally. A client
		// is refused outright for an entity it does not own: destroying it locally
		// while the host keeps replicating it desyncs this client permanently.
		void Despawn(World& world, Entity entity);

		// The half of Despawn that is NOT the destruction: run the refusal, broadcast
		// the despawn if this peer is the host, and unbind the net id. Returns whether
		// the caller should now destroy `entity`; false means the call was refused and
		// nothing happened.
		//
		// Split out for one caller: Net.Despawn from a script. A script runs inside the
		// script runner's own iteration over ScriptComponent storage, so destroying an
		// entity there frees the storage that loop is walking - which is exactly why
		// Entity.Destroy is deferred to the end of the script update. The wire half
		// must NOT be deferred with it (the other peers should hear about this now, and
		// the net id must stop resolving to an entity that is about to die), so the two
		// halves are separable here rather than at the call site.
		[[nodiscard]] bool ReleaseForDespawn(World& world, Entity entity);

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

		// ── Ownership transfer ───────────────────────────────────────────────
		// Hands `entity`'s NetworkIdentity::owner to `newOwner` - the only way an
		// already-spawned entity's owner ever changes outside of Spawn (its initial
		// value) and a disconnect's release (NetworkReceiveSystem::OnDisconnected).
		// `newOwner` is usually the caller's own LocalConnectionId() (claim) or
		// kInvalidConnection (release back to the host); see Net.RequestOwnership/
		// Net.ReleaseOwnership.
		//
		// A no-op success (OwnershipTransferOutcome::Applied, nothing sent) for an
		// entity with no NetworkIdentity or one not yet part of a session
		// (netId == 0) - there is no owner field for anyone to disagree about, so
		// this process already IS the owner, the same nullptr rule IsOwner uses.
		//
		// OFFLINE AND ON THE HOST this is synchronous: the field changes on this
		// call, and a host also broadcasts NetMessage::OwnershipTransfer so every
		// connection agrees. THE HOST'S OWN CALL IS NEVER VALIDATED - see
		// OwnershipTransferRefusal's comment for why "host always wins" is half of
		// this feature's rule, the other half (ValidateOwnershipRequest) being what
		// gates a CLIENT's request once it arrives over the wire.
		//
		// ON A CLIENT this only SENDS NetMessage::OwnershipRequest and returns
		// Requested - `entity`'s owner does not change here. It changes when (and
		// only when) the host's broadcast lands, through ApplyOwnershipTransfer
		// below - the same "ask, then wait for the authoritative answer" shape
		// Spawn/Despawn already give every other piece of session state.
		[[nodiscard]] OwnershipTransferOutcome RequestOwnershipTransfer(World& world, Entity entity,
		        ConnectionId newOwner);

		// Client-side application of a host-approved NetMessage::OwnershipTransfer -
		// the ApplySpawn/ApplyDespawn counterpart for this message kind. A no-op for
		// a net id this peer has no binding for (already gone, or never held here);
		// nothing here can create an entity, only re-own one that already exists.
		void ApplyOwnershipTransfer(World& world, std::uint32_t netId, ConnectionId newOwner);

		// ── Authority ────────────────────────────────────────────────────────
		// THE OWNER OF AN ENTITY IS AUTHORITATIVE FOR IT. That is the whole model:
		// the owning peer simulates its own entity and replicates the result, the
		// host relays it to everyone else, and no peer ever simulates - or corrects -
		// something it does not own.
		//
		// HasAuthority and IsOwner therefore give the same answer, and they are both
		// kept because they read as different questions at a call site: "may I decide
		// this entity's state this frame" (a simulation gate) and "is this entity
		// mine" (an input gate). They used to differ - the host was authoritative for
		// the whole world - and a game written against HasAuthority under that model
		// would silently start simulating other people's characters if this now
		// answered the old way.
		//
		// Offline every entity is local, so both are true and single-player code
		// written against either just works.
		[[nodiscard]] bool HasAuthority(World& world, Entity entity) const;
		[[nodiscard]] bool IsOwner(World& world, Entity entity) const;

		// ── Player names ─────────────────────────────────────────────────────
		// The name a player would end up with if it claimed `desired` right now:
		// `desired` itself when nothing else is using it, otherwise "desired (2)",
		// "desired (3)" and so on.
		//
		// ONLY EARLIER PLAYERS ARE OBSTACLES, and that asymmetry is what makes this
		// converge. Players are ordered by (owner connection id, entity id), the host
		// being connection 0 and therefore always first; a player yields to anybody
		// ahead of it in that order and ignores everybody behind. Two peers picking the
		// same name in the same frame therefore cannot both step aside - the later one
		// does - so re-running this every frame settles instead of oscillating, with no
		// round trip and no peer writing another peer's field.
		//
		// `desired` is sanitised the same way a disconnect reason is: it reaches a font.
		[[nodiscard]] std::string ResolveDisplayName(World& world, Entity self, std::string_view desired) const;

		// Resolve `desired` and write it onto `self`'s NetPlayer, adding the component
		// if it has none. Refused (returns empty, writes nothing) for an entity this
		// peer does not own: the owner of an entity is authoritative for it, and a
		// display name is state like any other.
		std::string ClaimPlayerName(World& world, Entity self, std::string_view desired);

		// The same rule against an identity the caller already holds. Exists because
		// the replication systems walk a view of NetworkIdentity and would otherwise
		// pay a registry lookup per entity per frame to ask a question they have the
		// answer to in hand - and because ONE definition of "mine" shared by the send
		// filter, the receive resolution and the physics handover is the only way
		// those three can never disagree.
		[[nodiscard]] bool OwnsIdentity(const NetworkIdentity& identity) const;

		// ── Simulation authority ─────────────────────────────────────────────
		// Aligns every replicated entity's body with who is allowed to simulate it,
		// and is the reason a client can see a remote character move at all. Covers
		// three shapes of "simulated by physics": a 2D RigidBody2DComponent, a 3D
		// RigidBodyComponent, and a 3D CharacterControllerComponent - a ragdoll is
		// several 3D RigidBodyComponents (see below), not a fourth shape.
		//
		// NetworkReceiveSystem runs FIRST in the frame and writes the replicated
		// transform; the physics system runs later and, for a DYNAMIC/Dynamic body,
		// writes the transform again from its own integration of a body this peer has
		// no authority over. The network's answer loses every frame, so a remotely
		// owned character stands still while its snapshots arrive perfectly.
		//
		// The fix is the body type both physics engines already have for exactly
		// this: a Kinematic body is TRANSFORM-DRIVEN - Physics2DSystem::
		// PushKinematicTargets / PhysicsSystem::PushKinematicTargets push the ECS
		// pose into Box2D/Jolt each frame, and each engine's SyncTransforms writes
		// back only for a Dynamic body - which is precisely replication's semantics.
		// So a body this peer does not own becomes Kinematic, and goes back to what
		// it was authored as the moment this peer does own it.
		//
		// A Character Controller has no body-type concept at all - it is not a Jolt
		// Body - so it uses the seam CharacterControllerComponent::locallySimulated
		// was built for: false makes StepCharacters a passive shadow that mirrors
		// whatever TransformComponent replication already wrote, instead of running
		// a second independent simulation of the same character. No marker/restore
		// bookkeeping needed for it - see the .cpp.
		//
		// A RAGDOLL IS FROZEN AND THAWED AS A UNIT, every bone together, never one
		// bone at a time. Only the ragdoll's root carries NetworkIdentity, so the
		// naive per-entity walk below can only ever find that one - but a ragdoll's
		// bones are jointed to each other, and a Hinge/Swing Twist constraint between
		// a teleporting Kinematic root and a still-Dynamic limb is exactly the
		// mixed-authority case a constraint solver was never asked to make sense of
		// (jitter at best, an explosion at worst). So handing over or reclaiming the
		// root walks RagdollComponent::bones and applies the same operation to every
		// other bone too - see the .cpp for exactly how.
		//
		// THE HOST HANDS OVER TOO, and that is the change client authority makes here.
		// Under host authority the host simulated the whole world, so it kept every
		// body Dynamic; now a client owns its own character, and a host that went on
		// integrating that body would fight the transforms arriving from its owner
		// every single frame. Whichever wrote last would win, and the two peers would
		// disagree for as long as they were in contact - which is exactly the shape of
		// the "a host-side push shoves a client's body on the host only" bug.
		//
		// Offline is untouched: nothing is owned by anybody else, so every body stays
		// exactly as authored.
		//
		// Called once per frame from the receive system rather than from each of the
		// events that can change authority (Spawn, Welcome, ownership release on a
		// disconnect): those are several call sites that must never be forgotten, and
		// a reconcile that only acts on divergence costs one view walk and cannot be.
		void SyncSimulationAuthority(World& world);

		// Undoes every handover SyncSimulationAuthority made - 2D, 3D, ragdoll bones,
		// and Character Controllers alike - restoring whatever each was authored as.
		// Called from Stop, so a player leaving a session and returning to
		// single-player does not find a character (or a crate, or a ragdoll) that no
		// longer falls.
		static void RestoreSimulationAuthority(World& world);

	private:
		// Reads network.stunHost/stunPort/turnHost/turnPort/turnUsername/turnPassword/
		// allowRelay straight off SettingsService and pushes them into m_traversalSession
		// through its SetStunServer/SetTurnServer seams, so the connect ladder is never a
		// second copy of those values that could drift from the one place a project (or
		// Net.ConfigureRelay) actually sets them. Called at the top of HostWithCode and
		// JoinByCode, which is early enough: neither backend nor socket exists yet.
		void ConfigureTraversalFromSettings();

		ServiceContainer& m_services;
		NetworkSubsystem m_transport;
		NetSession m_session;
		ReplicationSchema m_schema;
		RelevancySettings m_relevancy;
		int m_maxConnections = 0;
		std::string m_disconnectReason;
		float m_sendRateHz = 20.f;
		bool m_replicationReady = true;
		std::unordered_map<ConnectionId, SnapshotCache> m_caches;

		// Connections that have asked for the world again. Bounded by the connection
		// count (it is a set of live connection ids), emptied by Stop, and dropped per
		// connection by DropCacheFor - there is no queue of withheld entities anywhere,
		// on either peer, precisely so there is nothing that can grow or be applied to
		// the wrong world later.
		std::unordered_set<ConnectionId> m_resyncRequests;

		// When each connection last had a ClientReady HONOURED, so the request set
		// cannot be repopulated faster than the resyncs it triggers can be worth -
		// see RequestResync. Same lifetime as m_resyncRequests: dropped per
		// connection and emptied by Stop, because a Stop/StartHost cycle can hand a
		// reused peer id a stale floor.
		std::unordered_map<ConnectionId, float> m_lastResyncAt;

		std::chrono::steady_clock::time_point m_epoch = std::chrono::steady_clock::now();

		std::unique_ptr<ScriptFieldBridge> m_fieldBridge;
		std::unique_ptr<RpcBridge> m_rpcBridge;

		// Constructed against m_transport above, which it never outlives - see the
		// declaration order requirement on NetTraversalSession's own constructor.
		NetTraversalSession m_traversalSession;

		// Set by HostWithCode, consumed by the first TickTraversal that runs after
		// it - see that method's comment for why the delay is safe.
		bool m_traversalHostSetupPending = false;
	};
} // namespace aether::net
