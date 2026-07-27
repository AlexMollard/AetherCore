#include "net/NetworkSystems.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
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

		// Replay the world. A scene-placed entity the joiner already has resolves to
		// a net id it already knows and its ApplySpawn is a no-op; a prefab-spawned
		// one is instantiated. Either way the joiner ends the handshake bound to the
		// same net ids as everyone else.
		world.View<NetworkIdentity>().each(
		        [&](entt::entity ent, NetworkIdentity& identity)
		        {
			        if (identity.netId == 0)
			        {
				        return;
			        }
			        const Entity entity = World::FromEntt(ent);
			        const auto* transform = world.TryGet<TransformComponent>(entity);
			        const glm::vec3 position = transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(0.f);
			        const std::vector<std::byte> packet = EncodeSpawn(identity.netId, identity.owner,
			                identity.spawnPrefab, position);
			        context.Transport().Send(peer, kChannelReliable, true, packet);
		        });

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

		// Collect first: Despawn destroys entities, which invalidates the view.
		std::vector<Entity> owned;
		world.View<NetworkIdentity>().each(
		        [&](entt::entity ent, NetworkIdentity& identity)
		        {
			        if (identity.owner == peer && identity.netId != 0)
			        {
				        owned.push_back(World::FromEntt(ent));
			        }
		        });
		for (const Entity entity: owned)
		{
			context.Despawn(world, entity);
		}

		context.Session().RemoveConnection(peer);
		context.DropCacheFor(peer);
		AE_INFO(LogCategory::App, "Net: connection {} left ({} entity/entities despawned)", peer, owned.size());
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

		world.View<NetworkIdentity, NetworkTransform, TransformComponent>().each(
		        [&](entt::entity, NetworkIdentity& identity, NetworkTransform& tuning, TransformComponent& transform)
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
				        const glm::vec3 corrected = EaseToward(rendered.position, state.authoritativePosition,
				                tuning.correctionRate, dt, tuning.snapDistance);
				        transform.localToWorld = rendered.matrix;
				        transform.localToWorld[3] = glm::vec4(corrected, 1.f);
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
		if (now < m_nextSendTime)
		{
			return;
		}
		// Advance from `now` rather than by accumulating intervals: a long frame (a
		// scene load, a shader compile) must not leave the host owing a burst of
		// back-to-back snapshots it then fires on consecutive frames.
		m_nextSendTime = now + 1.f / rate;

		for (const ConnectionId connection: context.Session().Connections())
		{
			const glm::vec3 viewerPos = ViewerPosition(world, connection);
			const std::vector<Entity> relevant = RelevantWithTransformless(world, connection, viewerPos,
			        context.Relevancy());
			SnapshotCache& cache = context.CacheFor(connection);

			const std::vector<std::byte> snapshot = BuildSnapshot(world, context.Schema(), context.Catalog(),
			        context.Session(), cache, relevant);
			if (!snapshot.empty())
			{
				// Unreliable: a dropped snapshot is superseded by the next one, and
				// retransmitting stale state costs more than skipping it.
				context.Transport().Send(connection, kChannelSnapshot, false,
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

	glm::vec3 NetworkSendSystem::ViewerPosition(World& world, ConnectionId viewer)
	{
		// A connection sees from whatever it owns. The first owned entity with a
		// transform wins; a connection that owns nothing positioned yet views from
		// the origin, which only affects what it receives, never what it may own.
		glm::vec3 position{0.f};
		bool found = false;
		world.View<NetworkIdentity, TransformComponent>().each(
		        [&](entt::entity, NetworkIdentity& identity, TransformComponent& transform)
		        {
			        if (found || identity.owner != viewer)
			        {
				        return;
			        }
			        position = glm::vec3(transform.localToWorld[3]);
			        found = true;
		        });
		return position;
	}
} // namespace aether::net
