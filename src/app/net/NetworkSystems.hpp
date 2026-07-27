#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetInterpolation.hpp"
#include "net/NetTypes.hpp"
#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether::net
{
	class NetworkContext;
	class SnapshotCache;

	// Drains the transport and lands inbound authoritative state on the world.
	//
	// Registered FIRST, ahead of animation, physics and scripts: everything
	// downstream reads transforms and script fields this frame, and a system that
	// runs before the packet lands spends the whole frame simulating state the host
	// has already superseded.
	class NetworkReceiveSystem final : public System
	{
	public:
		explicit NetworkReceiveSystem(NetworkContext& context)
		      : m_context(context)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "NetworkReceiveSystem";
		}

		void Update(World& world, float dt) override;

		// The three transport-event handlers Update dispatches to. Public because
		// OnData is the framework's inbound trust boundary - the only place a remote
		// peer's bytes are gated by role - and reaching it through Update means
		// standing up a real socket, a peer and a handshake for every case in that
		// matrix. Calling them directly is precisely what Update does; this is a
		// reachable seam, not a separate test path.
		void OnConnected(World& world, ConnectionId peer);
		void OnDisconnected(World& world, ConnectionId peer);
		void OnData(World& world, ConnectionId peer, std::span<const std::byte> data);

	private:
		// Drops bindings whose entity no longer exists. A net id whose entity died by
		// a route replication never sees - a scene load, a script's Entity.Destroy -
		// would otherwise resolve to a dangling handle that an inbound packet hands
		// straight to try_get.
		void PruneDeadBindings(World& world);

		// Snapshot fields are written straight onto TransformComponent by
		// ApplySnapshot, which is right for the authoritative value and wrong for
		// what should be rendered. These two bracket the apply: the first records
		// where every replicated entity was, the second turns the difference into an
		// interpolation sample (remote entities) or an eased correction (the owned,
		// locally predicted one) and writes the result back.
		void CaptureRenderedTransforms(World& world);
		void ResolveTransforms(World& world, float dt);

		NetworkContext& m_context;

		// Last authoritative pose per net id, kept apart from the rendered transform:
		// a snapshot carries only the fields that changed, so reading "the transform"
		// after an apply that touched position but not rotation would sample the
		// interpolated rotation we ourselves wrote last frame.
		struct RemoteState
		{
			InterpolationBuffer buffer;
			glm::vec3 authoritativePosition{0.f};
			glm::vec3 authoritativeEuler{0.f};
		};

		struct Pose
		{
			// The exact pre-apply matrix, kept alongside the decomposed channels so the
			// owned-entity correction can restore rotation/scale bit-exact instead of
			// rebuilding them from a lossy decompose/recompose round trip.
			glm::mat4 matrix{1.f};
			glm::vec3 position{0.f};
			glm::vec3 euler{0.f};
			glm::vec3 scale{1.f};
		};

		std::unordered_map<std::uint32_t, RemoteState> m_remote;
		// Rebuilt every frame; a member only so the per-frame allocation is amortised.
		std::unordered_map<std::uint32_t, Pose> m_renderedBefore;
	};

	// Broadcasts post-simulation state to every connection.
	//
	// Registered LAST, after scripts and particles, so a client receives the world
	// as it ended the frame rather than half-updated: a snapshot built mid-frame
	// would carry positions physics is about to overwrite and script fields the
	// script has not set yet.
	class NetworkSendSystem final : public System
	{
	public:
		explicit NetworkSendSystem(NetworkContext& context)
		      : m_context(context)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "NetworkSendSystem";
		}

		void Update(World& world, float dt) override;

	private:
		[[nodiscard]] static glm::vec3 ViewerPosition(World& world, ConnectionId viewer);

		// Diffs `relevant` for `connection` against what was relevant to it a moment
		// ago. An entity that fell out (still alive elsewhere, just not bound here
		// any more - a destroyed one was already handled by Despawn) gets a
		// NetMessage::Relevancy so the connection stops rendering the frozen last-seen
		// copy, and that connection's SnapshotCache entry for it is cleared - the
		// change-detection state that would otherwise report "unchanged" on
		// re-entry, when the client that lost it needs the FULL current value
		// resent. An entity that appeared gets a fresh Spawn: the leave message
		// destroyed it client-side, and a Snapshot has nothing to write onto without
		// one.
		void UpdateRelevancyMembership(World& world, NetworkContext& context, ConnectionId connection,
		        const std::vector<Entity>& relevant, SnapshotCache& cache);

		// Drops tracking for a connection no longer in the session. Both maps below
		// are keyed by ConnectionId and outlive any one connection, so a long run of
		// join/leave churn - or a peer id reused after a Stop/StartHost cycle resets
		// NetworkSubsystem's id counter - would otherwise leak, or worse inherit,
		// another connection's stale state.
		void PruneDisconnected(const std::vector<ConnectionId>& live);

		NetworkContext& m_context;

		// Wall-clock deadline of the next send, PER CONNECTION rather than one shared
		// deadline for the whole host: a single deadline means every connection's
		// cadence shifts the instant any other one joins or leaves, since the next
		// fire time was computed from `now` at a moment with a different connection
		// count. Paced independently, each connection's own 20Hz cadence is stable
		// regardless of who else is connected. Wall clock, not accumulated frame dt,
		// so pausing or time-scaling the game does not silently change the send rate.
		std::unordered_map<ConnectionId, float> m_nextSendTimeByConnection;

		// Net ids relevant to each connection as of the last tick this system
		// actually sent to it - see UpdateRelevancyMembership.
		std::unordered_map<ConnectionId, std::unordered_set<std::uint32_t>> m_relevantNetIds;
	};
} // namespace aether::net
