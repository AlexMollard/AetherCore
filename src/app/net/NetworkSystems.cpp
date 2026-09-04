#include "net/NetworkSystems.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "net/NetRelevancy.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// Position comes straight off column 3 and survives a compose/decompose round
		// trip exactly; euler does not, so both are compared with a tolerance. An exact
		// comparison would report the rotation as "changed" every single frame purely
		// from the round trip through ComposeTransform.
		constexpr float kPoseEpsilon = 1e-4f;

		bool Differs(glm::vec3 a, glm::vec3 b)
		{
			return std::abs(a.x - b.x) > kPoseEpsilon || std::abs(a.y - b.y) > kPoseEpsilon
			       || std::abs(a.z - b.z) > kPoseEpsilon;
		}

		// Whether a client that destroyed this entity could get it back from a Spawn
		// message. A Spawn names a prefab, and ApplySpawn refuses an empty one - a
		// scene-placed entity is something "nothing generic can recreate", since the
		// framework does not know how the project loads scenes.
		//
		// Relevancy MUST consult this before telling a connection to forget anything.
		// A relevancy leave is a straight ecs::DestroyHierarchy client-side, and for an
		// entity with no prefab to rebuild from that delete is PERMANENT: the re-entry
		// Spawn is rejected on arrival, and the join replay only ever runs once. Stop()
		// and OnDisconnected both already treat `scenePlaced` as sacrosanct for exactly
		// this reason; relevancy was the one path that did not, so at the default
		// 60-unit radius every replicated scene entity a player walked away from was
		// deleted on that player's machine for the rest of the session.
		//
		// Both halves are checked, not just `scenePlaced`: the flag is only set by
		// AssignScenePlacedNetIds, so anything else that binds a net id without a prefab
		// name is equally unrecoverable and equally must not be despawned.
		[[nodiscard]] bool ClientCanRecreate(const NetworkIdentity& identity)
		{
			return !identity.scenePlaced && !identity.spawnPrefab.empty();
		}

		// How much of an inbound state packet this peer is willing to believe.
		//
		// On the HOST the sender is a client, which is authoritative for the entities
		// it owns and for nothing else - so the gate is the whole of the security
		// story for client authority, and it is applied per FIELD inside the apply
		// rather than per packet, because one packet can name many entities.
		//
		// On a CLIENT the sender is the host. There is exactly one link and the host
		// is the session's authority (it decides spawns, despawns and relevancy), so
		// there is no second candidate to distinguish it from and nothing to gate
		// against. Note this is not the host being trusted to move a client's own
		// character: the host never sends an entity's state back to its owner (see
		// NetworkSendSystem), and ResolveTransforms discards any correction to an
		// owned entity regardless of who sent it.
		[[nodiscard]] StateWriteGate InboundGate(const NetworkContext& context, ConnectionId peer)
		{
			return context.IsHost() ? StateWriteGate::OwnedBy(peer) : StateWriteGate::TrustAll();
		}

		// Whether this peer is a client whose game has said it is NOT standing in the
		// scene the session's entities belong to - see NetworkContext::SetReplicationReady.
		//
		// Everything describing the WORLD is refused while this holds, and refusing is
		// the whole fix: a Spawn applied here builds the entity into the menu the player
		// is looking at, and the scene change destroys it a frame later with nothing on
		// either peer aware that it has gone. Nothing is buffered instead, deliberately -
		// a queue would be state that can grow, go stale, or be applied to the wrong
		// world. The client asks for all of it again when it arrives.
		[[nodiscard]] bool HoldingReplication(const NetworkContext& context)
		{
			return context.IsClient() && !context.IsReplicationReady();
		}
	} // namespace

	// ── Receive ─────────────────────────────────────────────────────────────────

	void NetworkReceiveSystem::Update(World& world, float dt)
	{
		NetworkContext& context = m_context;
		if (!context.IsActive())
		{
			// Offline is the overwhelmingly common case (every single-player game, and
			// the editor). Cost it at one bool test.
			if (!m_remote.empty())
			{
				m_remote.clear();
			}
			return;
		}

		context.Transport().Poll();

		// Real seconds, not the wall clock interpolation below reads instead: the
		// traversal ladder's own timeouts (PortMapping's give-up window, the punch,
		// the wait for ENet's handshake once a path opens) are driven the same way
		// NatRendezvous already is. Run AFTER Poll() so a punch response or the ENet
		// CONNECT that follows one is visible to it the same frame it arrived in,
		// not one frame late.
		context.TickTraversal(world, dt);
		PruneDeadBindings(world);

		// BOTH ROLES render someone else's simulation now: a client renders every
		// other player, and the host renders every client's. So both need the
		// pre-apply pose recorded, and both need the apply resolved into
		// interpolation afterwards. Under host authority only a client did.
		CaptureRenderedTransforms(world);

		// Copied, not iterated in place: a client losing its link tears the session
		// down inside the handler, and Disconnect() clears the transport's event
		// vector - iterating that span while a handler can free it is a use-after-free
		// waiting for the first dropped connection.
		const std::vector<NetEvent> events(context.Transport().Events().begin(), context.Transport().Events().end());
		for (const NetEvent& event: events)
		{
			switch (event.kind)
			{
			case NetEvent::Kind::Connected:
				OnConnected(world, event.peer);
				break;
			case NetEvent::Kind::Disconnected:
				OnDisconnected(world, event.peer);
				break;
			case NetEvent::Kind::Data:
				OnData(world, event.peer, event.data);
				break;
			}
		}

		// After the inbound events, because a Welcome or a Spawn handled above is what
		// establishes authority in the first place, and the entity a Spawn just created
		// must be handed over on the frame it appears - Physics2DSystem runs later in
		// this same frame and would otherwise integrate it once before anything
		// noticed. Before ResolveTransforms only for readability; the two are
		// independent. Both are role-agnostic now: the host defers to a client's
		// ownership exactly as a client defers to the host's.
		context.SyncSimulationAuthority(world);
		ResolveTransforms(world);
	}

	void NetworkReceiveSystem::PruneDeadBindings(World& world)
	{
		std::vector<std::uint32_t> dead;
		for (const auto& [netId, entity]: m_context.Session().Bindings())
		{
			if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
			{
				dead.push_back(netId);
			}
		}
		for (const std::uint32_t netId: dead)
		{
			m_context.Session().Unbind(netId);
			m_context.ForgetNetId(netId);
			m_remote.erase(netId);
		}
	}

	void NetworkReceiveSystem::OnConnected(World& world, ConnectionId peer)
	{
		NetworkContext& context = m_context;
		if (!context.IsHost())
		{
			// A client's "connected" is its link to the host coming up. It has no
			// connection id of its own until the host's Welcome answers.
			return;
		}
		if (peer == kInvalidConnection)
		{
			return;
		}

		// The player cap, enforced before the joiner becomes part of the session at
		// all. It has to be here rather than in the ENet peer budget: the socket layer
		// refuses a peer over its budget by ignoring it, and a joiner that is ignored
		// cannot be told anything. Accepting the link and then closing it with a reason
		// is the difference between "the game would not start" and "the server is full".
		if (context.IsFull())
		{
			AE_INFO(LogCategory::App, "Net: refused connection {} - {} ({} already connected)", peer,
			        kReasonServerFull, context.Session().Connections().size());
			context.RefuseConnection(peer, kReasonServerFull);
			return;
		}

		context.Session().AddConnection(peer);
		const std::vector<std::byte> welcome = NetworkContext::EncodeWelcome(peer);
		context.Transport().Send(peer, kChannelReliable, true, welcome);

		// Replay the world, FILTERED BY THE JOINER'S RELEVANCY - the same set, from the
		// same viewer position, that the first send tick will compute a moment later.
		// An unfiltered replay hands the joiner every bound entity in the world; the
		// send system then diffs against only the relevant ones, so anything out of
		// range at join exists on that client and is in no connection's "previous" set,
		// which means it can never appear in the leave loop either. It sits frozen at
		// its join-time pose for the rest of the session - the exact symptom relevancy
		// exists to prevent.
		//
		// A scene-placed entity out of range is not lost by this: the client derives its
		// net id from the same scene file (AssignScenePlacedNetIds) and its ApplySpawn
		// was always a no-op. It is the prefab-spawned ones that are genuinely withheld
		// until they become relevant, which is what relevancy means.
		const glm::vec3 viewerPos = ViewerPosition(world, peer);
		const std::vector<Entity> relevant = RelevantWithTransformless(world, peer, viewerPos, context.Relevancy());
		for (const Entity entity: relevant)
		{
			const auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity == nullptr || identity->netId == 0)
			{
				continue;
			}
			const auto* transform = world.TryGet<TransformComponent>(entity);
			const glm::vec3 position = transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(0.f);
			const std::vector<std::byte> packet = EncodeSpawn(identity->netId, identity->owner,
			        identity->spawnPrefab, position);
			context.Transport().Send(peer, kChannelReliable, true, packet);
		}

		AE_INFO(LogCategory::App, "Net: connection {} joined", peer);
	}

	void NetworkReceiveSystem::OnDisconnected(World& world, ConnectionId peer)
	{
		NetworkContext& context = m_context;
		if (!context.IsHost())
		{
			// The link to the host dropped. Tear the session down rather than keep a
			// half-live client applying nothing.
			context.Stop(world);
			m_remote.clear();
			AE_INFO(LogCategory::App, "Net: disconnected from host");
			return;
		}

		// Collect first: Despawn destroys entities, which invalidates the view. A
		// scene-placed entity owned by the leaver is never destroyed here - it came
		// from the scene file, every other peer (and the next joiner) still expects
		// to find it there, and it has no spawn record to replay it from if it were
		// gone. Only its ownership is released. A session-spawned one has no scene
		// origin to fall back to, so it really is despawned. Mirrors the same
		// `scenePlaced` split Stop() makes when the whole session ends, for the same
		// reason - see the note there.
		std::vector<Entity> owned;
		std::vector<Entity> released;
		world.View<NetworkIdentity>().each(
		        [&](entt::entity ent, NetworkIdentity& identity)
		        {
			        if (identity.owner != peer || identity.netId == 0)
			        {
				        return;
			        }
			        if (identity.scenePlaced)
			        {
				        released.push_back(World::FromEntt(ent));
			        }
			        else
			        {
				        owned.push_back(World::FromEntt(ent));
			        }
		        });
		for (const Entity entity: released)
		{
			if (auto* identity = world.TryGet<NetworkIdentity>(entity))
			{
				identity->owner = kInvalidConnection;
			}
		}
		for (const Entity entity: owned)
		{
			context.Despawn(world, entity);
		}

		context.Session().RemoveConnection(peer);
		context.DropCacheFor(peer);
		AE_INFO(LogCategory::App, "Net: connection {} left ({} entity/entities despawned, {} released)", peer,
		        owned.size(), released.size());
	}

	void NetworkReceiveSystem::OnData(World& world, ConnectionId peer, std::span<const std::byte> data)
	{
		NetworkContext& context = m_context;
		if (data.empty())
		{
			return;
		}

		const auto kind = static_cast<NetMessage>(static_cast<std::uint8_t>(data[0]));
		const std::span<const std::byte> payload = data.subspan(1);

		// Everything below treats the payload as hostile: a peer can send any bytes on
		// any channel, so a message a peer has no business sending is dropped by role
		// here, and every decoder past this point is bounds-checked and reports failure
		// rather than throwing.
		switch (kind)
		{
		// State travels in both directions under client authority, so neither of the
		// next two can be gated by role the way the rest of this switch is. What
		// replaces the role gate is the OWNERSHIP gate, and it is strictly stronger:
		// the host accepts a client's state for the entities that client owns and
		// discards every other field in the same packet, whatever net ids the sender
		// chose to name. A client, whose only peer is the host, trusts what arrives -
		// the host is the session's authority and there is no second candidate for
		// where a snapshot on that link came from.
		case NetMessage::Snapshot:
			if (HoldingReplication(context))
			{
				// Nothing this peer is holding belongs to the session, so a net id in
				// this packet either resolves to nothing or - worse - to a menu entity
				// that happens to have been given the same derived id.
				return;
			}
			ApplySnapshot(world, context.Schema(), context.Catalog(), context.Session(), payload,
			        InboundGate(context, peer));
			return;

		case NetMessage::ScriptFields:
			if (HoldingReplication(context))
			{
				return; // same reason as Snapshot above
			}
			if (const ScriptFieldBridge* bridge = context.FieldBridge())
			{
				ApplyScriptFieldPacket(world, context.Session(), *bridge, payload, InboundGate(context, peer));
			}
			return;

		case NetMessage::Spawn:
		{
			if (context.IsHost())
			{
				return;
			}
			if (HoldingReplication(context))
			{
				// THE DEFECT THIS GUARD EXISTS FOR. Applied here, the entity is built
				// into whatever scene the player is looking at - a menu, during
				// "connecting..." - and destroyed with it. The host is asked for every
				// one of these again the moment this peer says it has arrived.
				AE_VERBOSE(LogCategory::App, "Net: ignoring a spawn - this peer is not in the session's scene yet");
				return;
			}
			ByteReader reader{payload};
			if (const std::optional<SpawnMessage> msg = DecodeSpawn(reader))
			{
				context.ApplySpawn(world, *msg);
			}
			return;
		}

		case NetMessage::Despawn:
		{
			if (context.IsHost())
			{
				return;
			}
			ByteReader reader{payload};
			if (const std::optional<std::uint32_t> netId = DecodeDespawn(reader))
			{
				m_remote.erase(*netId);
				context.ApplyDespawn(world, *netId);
			}
			return;
		}

		case NetMessage::Relevancy:
		{
			// Only the host decides what is relevant to whom; a client cannot tell
			// itself (or, worse, could try to tell the host) to forget something.
			if (context.IsHost())
			{
				return;
			}
			ByteReader reader{payload};
			if (const std::optional<std::uint32_t> netId = DecodeRelevancyLeave(reader))
			{
				m_remote.erase(*netId);
				context.ApplyRelevancyLeave(world, *netId);
			}
			return;
		}

		case NetMessage::Rpc:
		{
			// Both directions arrive here - a client's Server call on the host, and
			// the host's Client/Multicast calls on a client - so the role is NOT
			// checked at this level. ApplyRpc checks it instead, against the target
			// the packet carries, because "which direction is legal" is a property of
			// the call and not of the message kind. Its two gates (direction, then
			// ownership) are what make the inbound half safe: this is the only route
			// by which a client can affect host state at all.
			ByteReader reader{payload};
			const std::optional<RpcMessage> msg = DecodeRpc(reader);
			if (!msg.has_value())
			{
				return;
			}
			if (const RpcBridge* bridge = context.Rpcs())
			{
				ApplyRpc(world, context.Session(), *bridge, *msg, peer, context.IsHost());
			}
			return;
		}

		case NetMessage::Disconnect:
		{
			// Client-only. A client cannot end the host's session, and a host that
			// honoured this would be handing every peer a kill switch.
			if (context.IsHost())
			{
				return;
			}
			ByteReader reader{payload};
			const std::optional<std::string> reason = DecodeDisconnect(reader);
			if (!reason.has_value())
			{
				return;
			}
			// Recorded BEFORE the teardown, and Stop() deliberately leaves it alone -
			// this is the one fact distinguishing "the host threw me out / went away on
			// purpose" from "the link died", and the game reads it after the session is
			// already gone.
			context.SetDisconnectReason(*reason);
			AE_INFO(LogCategory::App, "Net: session ended by the host - {}",
			        reason->empty() ? "no reason given" : reason->c_str());
			// Torn down here rather than waiting for the ENet disconnect that follows:
			// the reason has arrived, so there is nothing left this link can tell us,
			// and a refused joiner should not spend the disconnect timeout looking
			// connected.
			context.Stop(world);
			m_remote.clear();
			return;
		}

		case NetMessage::ClientReady:
		{
			// Host-only: a client cannot tell another client anything, and a client
			// that honoured this would be resyncing a host that never asked.
			if (!context.IsHost())
			{
				return;
			}
			const std::vector<ConnectionId>& live = context.Session().Connections();
			if (std::find(live.begin(), live.end(), peer) == live.end())
			{
				// Not (or no longer) part of this session - a refused joiner still
				// holds a link long enough to be told why, and it must not be able to
				// queue work against a connection the session does not have.
				return;
			}
			context.RequestResync(peer);
			AE_INFO(LogCategory::App, "Net: connection {} is in the session's scene - resending the world", peer);
			return;
		}

		case NetMessage::Welcome:
		{
			if (!context.IsClient())
			{
				return; // a client cannot assign the host an id
			}
			if (context.Session().LocalConnection() != kInvalidConnection)
			{
				// Already welcomed. A repeated (or hostile) Welcome mid-session must
				// not run this again: ResetForNewSession would zero every already
				// -spawned entity's netId while their session bindings survive, and
				// a fresh AssignScenePlacedNetIds would then bind none of them.
				AE_VERBOSE(LogCategory::App, "Net: dropping unexpected Welcome from connection {} - already connected",
				        peer);
				return;
			}
			ByteReader reader{payload};
			const ConnectionId assigned = reader.U32();
			if (!reader.Ok() || assigned == kInvalidConnection)
			{
				return;
			}
			context.Transport().SetLocalConnectionId(assigned);
			context.Session().SetLocalConnection(assigned);
			// Derive the same ids for scene-placed entities the host derived, from the
			// same scene file, with no exchange - see AssignScenePlacedNetIds. Ids from
			// an earlier session are cleared first, or a reconnect would find every
			// entity already numbered and bind none of them.
			NetworkContext::ResetForNewSession(world);
			AssignScenePlacedNetIds(world, context.Session());
			AE_INFO(LogCategory::App, "Net: joined as connection {}", assigned);
			return;
		}
		}

		AE_VERBOSE(LogCategory::App, "Net: dropping unknown message {} from connection {}",
		        static_cast<std::uint32_t>(kind), peer);
	}

	void NetworkReceiveSystem::CaptureRenderedTransforms(World& world)
	{
		m_renderedBefore.clear();
		// Every replicated entity with a transform, NetworkTransform or not - see the
		// note on ResolveTransforms' view. An entity missing from this map is skipped
		// there, so narrowing this narrows the zero-correction guarantee with it.
		world.View<NetworkIdentity, TransformComponent>().each(
		        [&](entt::entity, NetworkIdentity& identity, TransformComponent& transform)
		        {
			        if (identity.netId == 0)
			        {
				        return;
			        }
			        Pose pose;
			        pose.matrix = transform.localToWorld;
			        DecomposeTRS(transform.localToWorld, pose.position, pose.euler, pose.scale);
			        m_renderedBefore[identity.netId] = pose;
		        });
	}

	void NetworkReceiveSystem::ResolveTransforms(World& world)
	{
		NetworkContext& context = m_context;
		const float now = context.Now();

		// NetworkTransform is looked up per entity rather than joined into the view,
		// because it is an OPTIONAL smoothing opt-in and the zero-correction rule
		// below is not optional. Joined, an entity without one would never reach this
		// system at all and an inbound packet naming the owner's own character would
		// land on its transform unopposed - the guarantee would quietly depend on a
		// component the project happened to author.
		world.View<NetworkIdentity, TransformComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, TransformComponent& transform)
		        {
			        if (identity.netId == 0)
			        {
				        return;
			        }
			        const auto before = m_renderedBefore.find(identity.netId);
			        if (before == m_renderedBefore.end())
			        {
				        return; // spawned by this frame's own packets; nothing to compare against yet
			        }
			        const Pose& rendered = before->second;

			        // ZERO CORRECTION ON THE OWNED ENTITY. This peer's own simulation is
			        // the truth for what it owns, so the pre-apply matrix is restored
			        // bit-exact and nothing else happens: no ease, no snap, no prediction
			        // machinery, no re-seating of the physics body.
			        //
			        // The restore is not redundant even though the send side never echoes an
			        // entity's state back to its owner. "The owner receives no correction"
			        // has to be true of what this machine DOES, not of what a well-behaved
			        // peer happens to send - a host running an older build, or one being
			        // hostile, must not be able to tug the local character by a millimetre.
			        //
			        // Restoring the MATRIX rather than recomposing from the decomposed
			        // channels matters: a decompose/recompose round trip rebuilds rotation
			        // and scale from lossy trig and re-injects float error into channels
			        // this branch never meant to touch (see the "position" field setter's
			        // note in CoreComponents.reflect.cpp).
			        if (context.OwnsIdentity(identity))
			        {
				        transform.localToWorld = rendered.matrix;
				        return;
			        }

			        const auto* tuning = world.TryGet<NetworkTransform>(World::FromEntt(ent));
			        if (tuning == nullptr)
			        {
				        // No smoothing asked for: the received value stands exactly as
				        // ApplySnapshot wrote it, which is the documented behaviour for a
				        // replicated entity with no NetworkTransform (it visibly snaps).
				        return;
			        }

			        Pose applied;
			        DecomposeTRS(transform.localToWorld, applied.position, applied.euler, applied.scale);

			        const auto [slot, inserted] = m_remote.try_emplace(identity.netId);
			        RemoteState& state = slot->second;
			        if (inserted)
			        {
				        state.authoritativePosition = rendered.position;
				        state.authoritativeEuler = rendered.euler;
			        }

			        // A snapshot carries only the fields that changed, so each channel is
			        // updated independently: a packet that moved the entity without turning
			        // it must not overwrite the last known rotation with the interpolated
			        // one this system wrote last frame.
			        bool changed = false;
			        if (Differs(applied.position, rendered.position))
			        {
				        state.authoritativePosition = applied.position;
				        changed = true;
			        }
			        if (Differs(applied.euler, rendered.euler))
			        {
				        state.authoritativeEuler = applied.euler;
				        changed = true;
			        }

			        if (changed)
			        {
				        state.buffer.Push(TransformSample{
				                .time = now,
				                .position = state.authoritativePosition,
				                .rotation = state.authoritativeEuler,
				        });
			        }

			        // Render the remote entity in the past, where there is a sample on both
			        // sides of the render time to interpolate between. This is what makes
			        // OTHER players look smooth between snapshots, and it is untouched by the
			        // move to client authority - only where the samples originate changed.
			        const float delay = std::max(0.f, tuning->interpolationDelaySeconds);
			        const std::optional<TransformSample> sample = state.buffer.Sample(now - delay);
			        if (!sample.has_value())
			        {
				        // Nothing authoritative yet: keep what was on screen rather than
				        // snapping the entity to whatever the apply left behind.
				        transform.localToWorld = ComposeTransform(rendered.position, rendered.euler, applied.scale);
				        return;
			        }
			        transform.localToWorld = ComposeTransform(sample->position, sample->rotation, applied.scale);
		        });
	}

	// ── Send ────────────────────────────────────────────────────────────────────

	// EVERY PEER REPLICATES WHAT IT OWNS. This used to return early off the host -
	// state flowed one way and a client spoke only through RPCs - and that is the line
	// the move to client authority deletes. What replaces it is a per-entity ownership
	// filter on both sides of the link:
	//
	//   a client sends the entities it owns, to the host, and nothing else;
	//   the host sends each connection everything relevant to it EXCEPT what that
	//   connection owns, which is both the relay of other clients' state and the
	//   guarantee that nobody is ever told where their own character is.
	//
	// The second half is what makes "zero correction on the owned entity" true on the
	// wire rather than only in the receiver.
	namespace
	{
		// Bounds how long a client can hold a stale value for an entity that has stopped
		// changing, when the diff carrying its last update was dropped. Long enough that the
		// extra full state send is negligible beside the per-tick diffs, short enough that a
		// desync is not something a player experiences.
		constexpr float kResyncIntervalSeconds = 1.0f;
	} // namespace

	void NetworkSendSystem::Update(World& world, float dt)
	{
		(void) dt; // paced off the wall clock, not the (pausable, scalable) frame delta

		NetworkContext& context = m_context;
		if (!context.IsConnected())
		{
			// Offline, or a client whose Welcome has not landed: it has no connection
			// id yet, so it does not know what it owns and must not guess.
			return;
		}

		StampOwnedPing(world, context);

		if (context.IsClient())
		{
			SendOwnedToHost(world, context);
			return;
		}

		const float now = context.Now();
		const float rate = std::max(1.f, context.SendRateHz());

		PruneDisconnected(context.Session().Connections());

		for (const ConnectionId connection: context.Session().Connections())
		{
			float& nextSend = m_nextSendTimeByConnection[connection];
			if (now < nextSend)
			{
				continue;
			}
			// Advance from `now` rather than by accumulating intervals: a long frame (a
			// scene load, a shader compile) must not leave this connection owing a
			// burst of back-to-back snapshots it then fires on consecutive frames.
			nextSend = now + 1.f / rate;

			const glm::vec3 viewerPos = ViewerPosition(world, connection);
			const std::vector<Entity> relevant = RelevantWithTransformless(world, connection, viewerPos,
			        context.Relevancy());
			SnapshotCache& cache = context.CacheFor(connection);

			// This connection has just told us it is standing in the session's scene and
			// holding nothing from the one it was in before (NetMessage::ClientReady).
			// Forget BOTH halves of what it was believed to have, and the machinery below
			// does the rest with no second replay path: every relevant entity reads as
			// newly admitted, so the ones a client can rebuild get a Spawn and the whole
			// state goes out behind them as one RELIABLE full snapshot instead of a diff
			// against values that connection threw away. Consumed here rather than on
			// arrival so a request that lands between two paced sends is still acted on.
			if (context.ConsumeResyncRequest(connection))
			{
				m_relevantNetIds[connection].clear();
				cache.Clear();
			}

			// Relevancy membership is computed over the UNFILTERED set, and must be: a
			// connection still needs the Spawn for its own character, and would never
			// be admitted at all if its own entity were filtered out here. Only the
			// STATE is filtered.
			// Periodic full state resend. Forgetting only the CACHE (never relevancy) makes
			// the next snapshot a full state write without replaying Spawns, and marking the
			// tick admitted sends it reliably - the same "a resync is not a diff, so it must
			// not be droppable" reasoning as the admission path above.
			float& nextResync = m_nextResyncByConnection[connection];
			const bool periodicResync = now >= nextResync;
			if (periodicResync)
			{
				nextResync = now + kResyncIntervalSeconds;
				cache.Clear();
			}

			const bool admitted = UpdateRelevancyMembership(world, context, connection, relevant, cache) || periodicResync;

			const std::vector<Entity> replicated = ExceptOwnedBy(world, relevant, connection);

			const std::vector<std::byte> snapshot = BuildSnapshot(world, context.Schema(), context.Catalog(),
			        context.Session(), cache, replicated);
			if (!snapshot.empty())
			{
				// Normally unreliable: a dropped snapshot is superseded by the next
				// one, and retransmitting stale state costs more than skipping it.
				//
				// A tick that ADMITTED an entity is the exception, and the reason is
				// cache.Forget: forgetting is what makes this snapshot a FULL state
				// send rather than a diff, and BuildSnapshot has already recorded
				// every value it just wrote as sent - so nothing resends them. Dropped
				// unreliably, every replicated field except the position the Spawn
				// carried would sit at prefab default on that client until it happened
				// to change again. "The next one supersedes it" is true of a diff and
				// false of a resync, so the resync goes reliable.
				context.Transport().Send(connection, admitted ? kChannelReliable : kChannelSnapshot, admitted,
				        NetworkContext::Frame(NetMessage::Snapshot, snapshot));
			}

			if (const ScriptFieldBridge* bridge = context.FieldBridge())
			{
				const std::vector<std::byte> fields = BuildScriptFieldPacket(world, context.Session(), cache,
				        *bridge, replicated);
				if (!fields.empty())
				{
					// Reliable: script fields are per-field change-detected, so a lost
					// packet is a value that is never resent until it changes again.
					context.Transport().Send(connection, kChannelReliable, true,
					        NetworkContext::Frame(NetMessage::ScriptFields, fields));
				}
			}
		}
	}

	void NetworkSendSystem::StampOwnedPing(World& world, const NetworkContext& context)
	{
		const auto ping = context.LocalRoundTripMs();
		world.View<NetworkIdentity, NetPlayer>().each(
		        [&](entt::entity, NetworkIdentity& identity, NetPlayer& player)
		        {
			        if (identity.netId == 0 || !context.OwnsIdentity(identity))
			        {
				        return;
			        }
			        // Written unconditionally rather than only on change: the value is one
			        // word, and the snapshot layer's own change detection is what decides
			        // whether it costs anything on the wire. A "has it moved" test here
			        // would be a second, weaker copy of that.
			        player.pingMs = ping;
		        });
	}

	// A client's whole send path. No relevancy: the host is not a viewer with a
	// radius, it is the relay every other peer's copy of these entities comes from,
	// so an owned entity is always relevant to it. No per-connection loop either -
	// a client has exactly one link, addressed as kInvalidConnection, which is how
	// the transport already spells "the host".
	void NetworkSendSystem::SendOwnedToHost(World& world, NetworkContext& context)
	{
		const float now = context.Now();
		const float rate = std::max(1.f, context.SendRateHz());

		float& nextSend = m_nextSendTimeByConnection[kInvalidConnection];
		if (now < nextSend)
		{
			return;
		}
		nextSend = now + 1.f / rate;

		const std::vector<Entity> owned = OwnedEntities(world, context);
		if (owned.empty())
		{
			return;
		}
		SnapshotCache& cache = context.CacheFor(kInvalidConnection);

		const std::vector<std::byte> snapshot = BuildSnapshot(world, context.Schema(), context.Catalog(),
		        context.Session(), cache, owned);
		if (!snapshot.empty())
		{
			// Unreliable for the same reason the host's diffs are: a dropped upload is
			// superseded by the next one 50 ms later, and a retransmitted position is
			// stale by the time it lands. There is no admitted/resync case on this side
			// - a client never spawns anything, so nothing is ever forgotten and
			// re-sent in full.
			context.Transport().Send(kInvalidConnection, kChannelSnapshot, false,
			        NetworkContext::Frame(NetMessage::Snapshot, snapshot));
		}

		if (const ScriptFieldBridge* bridge = context.FieldBridge())
		{
			const std::vector<std::byte> fields = BuildScriptFieldPacket(world, context.Session(), cache, *bridge,
			        owned);
			if (!fields.empty())
			{
				context.Transport().Send(kInvalidConnection, kChannelReliable, true,
				        NetworkContext::Frame(NetMessage::ScriptFields, fields));
			}
		}
	}

	std::vector<Entity> NetworkSendSystem::OwnedEntities(World& world, const NetworkContext& context)
	{
		std::vector<Entity> owned;
		world.View<NetworkIdentity>().each(
		        [&](entt::entity ent, NetworkIdentity& identity)
		        {
			        if (identity.netId == 0 || !context.OwnsIdentity(identity))
			        {
				        return;
			        }
			        owned.push_back(World::FromEntt(ent));
		        });
		return owned;
	}

	std::vector<Entity> NetworkSendSystem::ExceptOwnedBy(World& world, const std::vector<Entity>& entities,
	        ConnectionId owner)
	{
		std::vector<Entity> kept;
		kept.reserve(entities.size());
		for (const Entity entity: entities)
		{
			const auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity != nullptr && identity->owner == owner)
			{
				continue;
			}
			kept.push_back(entity);
		}
		return kept;
	}

	bool NetworkSendSystem::UpdateRelevancyMembership(World& world, NetworkContext& context, ConnectionId connection,
	        const std::vector<Entity>& relevant, SnapshotCache& cache)
	{
		std::unordered_set<std::uint32_t> currentIds;
		currentIds.reserve(relevant.size());
		for (const Entity entity: relevant)
		{
			const std::uint32_t netId = context.Session().NetIdFor(entity);
			if (netId != 0)
			{
				currentIds.insert(netId);
			}
		}

		// No first-tick special case. A connection starts with an empty "previous"
		// set, which is exactly true: OnConnected's replay is filtered through the
		// SAME relevancy call this tick makes, so the only entities the joiner has are
		// ones the re-entry loop below would send anyway - and ApplySpawn on a net id
		// the client is already bound to is a documented no-op. The alternative, a
		// suppressed first tick, only holds while the replay and the tick agree about
		// what is relevant, and they stop agreeing the moment the joiner acquires an
		// owned entity between the two (its viewer position moves, so the sets differ)
		// - at which point the difference is silently never sent.
		std::unordered_set<std::uint32_t>& previousIds = m_relevantNetIds[connection];

		// Left: still bound (alive), but no longer in this connection's current set. A
		// netId no longer bound at all was destroyed outright - Despawn already told
		// this connection about that, so there is nothing left to say here.
		for (const std::uint32_t netId: previousIds)
		{
			if (currentIds.contains(netId))
			{
				continue;
			}
			const Entity entity = context.Session().EntityFor(netId);
			if (!entity.IsValid())
			{
				continue;
			}
			const auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity == nullptr || !ClientCanRecreate(*identity))
			{
				// Unrecreatable client-side: a leave here is a permanent delete. See
				// ClientCanRecreate. Nothing is sent and nothing is forgotten - the
				// client still has the entity, so the cache still describes what it
				// holds and the next diff that matters is still correct.
				continue;
			}
			context.Transport().Send(connection, kChannelReliable, true, EncodeRelevancyLeave(netId));
			// Or the next re-entry's snapshot would see "unchanged" against a cached
			// value this connection was told to forget, and send nothing.
			cache.Forget(netId);
		}

		// Re-entered (or newly relevant): a Snapshot alone has nothing client-side to
		// write onto once the leave message destroyed the entity there - only a Spawn
		// brings it back.
		bool admitted = false;
		for (const std::uint32_t netId: currentIds)
		{
			if (previousIds.contains(netId))
			{
				continue;
			}
			const Entity entity = context.Session().EntityFor(netId);
			const auto* identity = entity.IsValid() ? world.TryGet<NetworkIdentity>(entity) : nullptr;
			if (identity == nullptr || !ClientCanRecreate(*identity))
			{
				// The mirror of the skip above, and it must be the SAME predicate: one
				// of these was never sent a leave, so it was never destroyed, so there
				// is nothing to bring back - and a Spawn naming no prefab is rejected
				// by ApplySpawn regardless.
				continue;
			}
			const auto* transform = world.TryGet<TransformComponent>(entity);
			const glm::vec3 position = transform != nullptr ? glm::vec3(transform->localToWorld[3])
			                                                 : glm::vec3(0.f);
			context.Transport().Send(connection, kChannelReliable, true,
			        EncodeSpawn(netId, identity->owner, identity->spawnPrefab, position));
			admitted = true;
		}

		previousIds = std::move(currentIds);
		return admitted;
	}

	void NetworkSendSystem::PruneDisconnected(const std::vector<ConnectionId>& live)
	{
		const std::unordered_set<ConnectionId> liveSet(live.begin(), live.end());
		std::erase_if(m_nextSendTimeByConnection, [&](const auto& kv) { return !liveSet.contains(kv.first); });
		std::erase_if(m_nextResyncByConnection, [&](const auto& kv) { return !liveSet.contains(kv.first); });
		std::erase_if(m_relevantNetIds, [&](const auto& kv) { return !liveSet.contains(kv.first); });
	}
} // namespace aether::net
