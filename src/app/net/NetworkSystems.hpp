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
	// How many NetMessage::Rpc packets one connection may land on a host per
	// second. A Server RPC is the one inbound packet that costs the host real work
	// per call - a script scan, a handle lookup, a managed reflection dispatch -
	// while costing the sender almost nothing, and a handler that relays via a
	// Multicast turns each accepted call into a packet per connection. This is a
	// cost ceiling rather than a gameplay knob: far past any honest game's RPC
	// traffic, and only ever consulted on the host, whose cost is the one being
	// defended.
	inline constexpr int kMaxInboundRpcsPerSecond = 64;

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

		// One admitted-Rpc-per-window question for `peer`, host side only - see
		// kMaxInboundRpcsPerSecond. A fixed window rather than anything cleverer: the
		// only property that matters is that a stale entry can never outlive its
		// window, so a connection id reused after a Stop/StartHost cycle inherits at
		// most one window's worth of suppression and then a clean slate.
		bool AdmitInboundRpc(ConnectionId peer);

		struct RpcWindow
		{
			float start = 0.f;
			int admitted = 0;
		};

		std::unordered_map<ConnectionId, RpcWindow> m_rpcWindows;

		// Snapshot fields are written straight onto TransformComponent by
		// ApplySnapshot, which is right for the authoritative value and wrong for
		// what should be rendered. These two bracket the apply: the first records
		// where every replicated entity was, the second turns the difference into an
		// interpolation sample (entities owned by somebody else) or discards it
		// outright (an entity owned HERE, which receives no correction at all) and
		// writes the result back.
		void CaptureRenderedTransforms(World& world);
		void ResolveTransforms(World& world);

		NetworkContext& m_context;

		// Last received pose per net id, kept apart from the rendered transform:
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
			// The exact pre-apply matrix, kept alongside the decomposed channels so an
			// owned entity can be restored bit-exact instead of rebuilt from a lossy
			// decompose/recompose round trip.
			glm::mat4 matrix{1.f};
			glm::vec3 position{0.f};
			glm::vec3 euler{0.f};
			glm::vec3 scale{1.f};
		};

		std::unordered_map<std::uint32_t, RemoteState> m_remote;
		// Rebuilt every frame; a member only so the per-frame allocation is amortised.
		std::unordered_map<std::uint32_t, Pose> m_renderedBefore;
	};

	// Uploads this peer's post-simulation state for the entities it OWNS.
	//
	// On a host that means every connection is sent what is relevant to it minus
	// whatever that connection owns; on a client it means one upload of its own
	// entities to the host, which relays them onward. Both halves run here rather
	// than in two systems because they are the same three steps (pace, choose the
	// entity set, build and send) differing only in the set.
	//
	// Registered LAST, after scripts and particles, so a receiver gets the world as
	// it ended the frame rather than half-updated: a snapshot built mid-frame would
	// carry positions physics is about to overwrite and script fields the script has
	// not set yet.
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

		// Connections this system is still tracking state for, split by map so each
		// half of PruneDisconnected is separately observable. Both maps are keyed by
		// ConnectionId and outlive any one connection, so "the entry went away when the
		// connection did" is a real invariant with no other way to see it from outside.
		[[nodiscard]] std::size_t PacedConnectionCount() const
		{
			return m_nextSendTimeByConnection.size();
		}

		[[nodiscard]] std::size_t TrackedRelevancyCount() const
		{
			return m_relevantNetIds.size();
		}

	private:
		[[nodiscard]] bool UpdateRelevancyMembership(World& world, NetworkContext& context, ConnectionId connection,
		        const glm::vec3 viewerPos, std::vector<Entity>& relevant, SnapshotCache& cache);

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
		//
		// An entity the client could not rebuild from a Spawn (scene-placed, or any
		// other binding with no prefab name) is exempt from BOTH halves - see
		// ClientCanRecreate in the .cpp.
		//
		// Returns whether anything was admitted this tick, which is what makes the
		// snapshot that follows a full resync rather than a diff - and therefore what
		// decides the channel it goes out on.

		// A client's entire send path: what it owns, once, to the host. Separate from
		// Update's host loop rather than folded into it because the two share no
		// relevancy, no per-connection iteration and no spawn/leave bookkeeping - a
		// client never admits anything to anybody.
		void SendOwnedToHost(World& world, NetworkContext& context);

		// Every bound entity this peer owns. The single source of "what do I
		// replicate" on a client, and deliberately the same OwnsIdentity predicate
		// the receive system and the physics handover use.
		[[nodiscard]] static std::vector<Entity> OwnedEntities(World& world, const NetworkContext& context);

		// Write this peer's own link cost onto every NetPlayer it owns, so the value
		// goes out with that entity's other replicated fields.
		//
		// HERE, rather than left to each project, because the only peer that can measure
		// a link is one of its two ends, and the only entity it may write is one it owns
		// - which together leave exactly one correct implementation. A game writing it
		// anywhere else is writing a field the owner overwrites (the "some player names
		// never showed" defect in a different disguise), and a game measuring it any
		// other way is adding packets to answer a question the transport already
		// answered for free.
		//
		// Called immediately before the send so the stamped value and the packet that
		// carries it come from the same tick, and only on entities that already carry a
		// NetPlayer: this must not decide that everything replicated is a player.
		static void StampOwnedPing(World& world, const NetworkContext& context);

		// `entities` minus the ones `owner` owns - the host's relay filter. A
		// connection is authoritative for its own entities, so sending their state
		// back would be the host arguing with them, which is precisely the round trip
		// client authority exists to delete.
		[[nodiscard]] static std::vector<Entity> ExceptOwnedBy(World& world, const std::vector<Entity>& entities,
		        ConnectionId owner);

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
		// Snapshots are diffs sent unreliably, and BuildSnapshot records every value it
		// writes as sent. "A dropped snapshot is superseded by the next one" therefore holds
		// only while an entity keeps changing: lose the LAST update before it goes idle and
		// the cache believes that value was delivered, nothing further is ever queued for it,
		// and the client stays wrong forever. A player who stops walking is the common case.
		// A periodic full state resend bounds that to one interval.
		std::unordered_map<ConnectionId, float> m_nextResyncByConnection;

		// Net ids relevant to each connection as of the last tick this system
		// actually sent to it - see UpdateRelevancyMembership.
		std::unordered_map<ConnectionId, std::unordered_set<std::uint32_t>> m_relevantNetIds;
	};
} // namespace aether::net
