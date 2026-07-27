#include "net/NetworkSystems.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <vector>

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "net/NetInput.hpp"
#include "net/NetRelevancy.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetSpawn.hpp"
#include "net/NetworkContext.hpp"
#include "physics2d/Physics2DSystem.hpp"
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
		PruneDeadBindings(world);

		// Only a client renders someone else's simulation, so only a client has a
		// rendered pose to preserve across the apply.
		if (context.IsClient())
		{
			CaptureRenderedTransforms(world);
		}

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

		if (context.IsClient())
		{
			// After the inbound events, because a Welcome or a Spawn handled above is
			// what establishes authority in the first place, and the entity a Spawn just
			// created must be handed over on the frame it appears - Physics2DSystem runs
			// later in this same frame and would otherwise integrate it once before
			// anything noticed. Before ResolveTransforms only for readability; the two
			// are independent.
			context.SyncSimulationAuthority(world);
			ResolveTransforms(world, dt);
		}
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
		case NetMessage::Snapshot:
			if (context.IsHost())
			{
				return; // only the host is authoritative; a client cannot rewrite its world
			}
			ApplySnapshot(world, context.Schema(), context.Catalog(), context.Session(), payload);
			return;

		case NetMessage::ScriptFields:
			if (context.IsHost())
			{
				return;
			}
			if (const ScriptFieldBridge* bridge = context.FieldBridge())
			{
				ApplyScriptFieldPacket(world, context.Session(), *bridge, payload);
			}
			return;

		case NetMessage::Spawn:
		{
			if (context.IsHost())
			{
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

		case NetMessage::Input:
		{
			// One direction only, unlike Rpc: input travels client-to-host and nothing
			// else, so the role gate belongs here and there is no target byte for a
			// sender to choose. A client receiving one is a peer trying to drive this
			// machine's simulation, which nothing is ever allowed to do.
			if (!context.IsHost())
			{
				return;
			}
			ByteReader reader{payload};
			const std::optional<InputMessage> msg = DecodeInput(reader);
			if (!msg.has_value())
			{
				return;
			}
			if (const RpcBridge* bridge = context.Rpcs())
			{
				// The ownership gate (a client may drive only what it owns) and the
				// staleness gate both live in ApplyInput.
				ApplyInput(world, context.Session(), *bridge, *msg, peer, context.InputGate());
			}
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
		world.View<NetworkIdentity, NetworkTransform, TransformComponent>().each(
		        [&](entt::entity, NetworkIdentity& identity, NetworkTransform&, TransformComponent& transform)
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

	void NetworkReceiveSystem::ResolveTransforms(World& world, float dt)
	{
		NetworkContext& context = m_context;
		const ConnectionId local = context.Session().LocalConnection();
		const float now = context.Now();

		// Entities whose predicted pose this tick actually moved. Collected rather
		// than pushed into physics inline: the push is what makes the correction
		// survive the frame at all (see the note below), and it needs one lookup of
		// the physics system rather than one per entity.
		std::vector<Entity> corrected;

		world.View<NetworkIdentity, NetworkTransform, TransformComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, NetworkTransform& tuning, TransformComponent& transform)
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

			        // Before the host's Welcome arrives this client has no id of its own,
			        // and kInvalidConnection would match every host-owned entity - so
			        // nothing counts as locally predicted until the session is real.
			        if (local != kInvalidConnection && identity.owner == local)
			        {
				        // The locally predicted entity. Scripts drove it to `rendered` this
				        // frame; ease it toward the host's answer instead of snapping, and
				        // leave the rotation alone - yanking the local player's facing to a
				        // stale authoritative value is worse than a small positional error.
				        // Only position changes here, so restore the pre-apply matrix
				        // bit-exact and touch nothing but its translation column: a
				        // decompose/recompose round trip would rebuild rotation and scale
				        // from lossy trig and re-inject float error into channels this
				        // branch never meant to touch (see the "position" field setter's
				        // note in CoreComponents.reflect.cpp).
				        const glm::vec3 eased = EaseToward(rendered.position, state.authoritativePosition,
				                tuning.correctionRate, dt, tuning.snapDistance);
				        transform.localToWorld = rendered.matrix;
				        transform.localToWorld[3] = glm::vec4(eased, 1.f);
				        if (Differs(eased, rendered.position))
				        {
					        corrected.push_back(World::FromEntt(ent));
				        }
				        return;
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
			        // sides of the render time to interpolate between.
			        const float delay = std::max(0.f, tuning.interpolationDelaySeconds);
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

		PushCorrectionsToPhysics(world, corrected);
	}

	// Why this exists at all, and why NOTHING above it was enough on its own.
	//
	// A locally-owned 2D body deliberately stays DYNAMIC on a client - it is the one
	// entity this peer predicts, so SyncSimulationAuthority leaves it on local
	// simulation (see the note there). Dynamic is also the exact body type
	// Physics2DSystem::SyncTransforms writes the transform back FOR, from the Box2D
	// pose, later in the same frame. So the ease/snap computed above was being
	// recomputed correctly every single tick and then overwritten before anything
	// could render it: the owner branch ran, the correction was real, and the frame
	// ended with the body exactly where prediction had put it. That is why an error
	// of fourteen units survived thirty seconds against a snapDistance of four.
	//
	// TeleportToTransform is the engine's existing answer to "something outside
	// physics moved this transform" - it sets the Box2D pose AND the interpolation
	// state, which is what stops the write-back from undoing it. Its own header
	// comment says as much.
	//
	// Only entities whose correction actually MOVED them are pushed: a prediction
	// that already agrees with the host must not have its body woken and re-seated
	// every frame just to arrive where it already was.
	void NetworkReceiveSystem::PushCorrectionsToPhysics(World& world, const std::vector<Entity>& corrected)
	{
		if (corrected.empty())
		{
			return;
		}
		// A world with no 2D physics registered (a 3D scene, a headless test) is not
		// an error: there is no body fighting the transform, so writing it was already
		// the whole of the correction.
		auto* physics = static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem"));
		if (physics == nullptr)
		{
			return;
		}
		for (const Entity entity: corrected)
		{
			if (world.Has<RigidBody2DComponent>(entity))
			{
				physics->TeleportToTransform(world, entity);
			}
		}
	}

	// ── Send ────────────────────────────────────────────────────────────────────

	void NetworkSendSystem::Update(World& world, float dt)
	{
		(void) dt; // paced off the wall clock, not the (pausable, scalable) frame delta

		NetworkContext& context = m_context;
		if (!context.IsHost())
		{
			return; // only the host replicates state; a client speaks through RPCs
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

			const bool admitted = UpdateRelevancyMembership(world, context, connection, relevant, cache);

			const std::vector<std::byte> snapshot = BuildSnapshot(world, context.Schema(), context.Catalog(),
			        context.Session(), cache, relevant);
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
				        *bridge, relevant);
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
		std::erase_if(m_relevantNetIds, [&](const auto& kv) { return !liveSet.contains(kv.first); });
	}
} // namespace aether::net
