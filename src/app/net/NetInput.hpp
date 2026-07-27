#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "net/NetRpc.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetSpawn.hpp" // NetMessage
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	class NetSession;

	// ── Client -> host input ────────────────────────────────────────────────────
	//
	// The missing half of a host-authoritative session. State replication is
	// deliberately one-way (NetworkSendSystem::Update returns early off the host), so
	// without this a client's player is simulated on the host as an unattended body
	// and every other peer sees it standing on its spawn marker forever.
	//
	// WHAT THE FRAMEWORK OWNS: moving an opaque payload from the peer that OWNS an
	// entity to the host, at a paced rate, on a channel where a stale packet is
	// dropped rather than retransmitted - and dispatching it, on arrival, to the
	// script method the sender named.
	//
	// WHAT THE GAME OWNS: what is in the payload and what the method does with it.
	// The framework cannot know what "jump" means, so it never looks inside.
	//
	// WHY IT IS NOT AN RPC, HAVING REUSED ALMOST ALL OF ONE. Addressing (net id +
	// script type hash + method index), the ownership gate and the dispatch are the
	// RPC path verbatim - ApplyInput below delegates to ApplyRpc rather than
	// reimplementing any of it, and the handler is declared with the same
	// [NetRpc(Server)] attribute. What differs is DELIVERY, and it differs in three
	// ways that are wrong to force onto every RPC:
	//   - RPCs are reliable-ordered. Input is worthless once it is stale, and a lost
	//     input packet that head-of-line-blocks the next five is strictly worse than
	//     one that is simply dropped.
	//   - Input is a continuous stream, so it needs a sequence number to reject a
	//     reordered arrival. A one-shot RPC has nothing to be stale relative to.
	//   - Input is sampled every frame, so it needs pacing. An RPC is sent when the
	//     game says so, once.
	// So it is a distinct wire kind with its own channel, carried by the same
	// machinery - the same call the Relevancy kind makes against Despawn, and for the
	// same reason: two facts that a receiver may one day need to tell apart must not
	// share a byte.

	struct InputMessage
	{
		std::uint32_t netId = 0;
		std::uint32_t scriptTypeHash = 0;
		std::uint16_t methodIndex = 0;
		// Per-entity, monotonic, incremented only on an actual send. The host applies
		// a message only if it is strictly newer than the last one it applied for that
		// entity, which is what makes a reordered arrival a no-op instead of a
		// permanent regression to older input.
		std::uint32_t sequence = 0;
		std::vector<std::byte> payload;
	};

	// Self-framing: leads with its own NetMessage byte, like Spawn/Rpc/Welcome.
	[[nodiscard]] std::vector<std::byte> EncodeInput(std::uint32_t netId, std::uint32_t scriptTypeHash,
	        std::uint16_t methodIndex, std::uint32_t sequence, std::span<const std::byte> payload);
	[[nodiscard]] std::optional<InputMessage> DecodeInput(ByteReader& r);

	// ── Send side ───────────────────────────────────────────────────────────────

	struct InputSend
	{
		// False = the payload is unchanged and the pacer has not come round yet, so
		// nothing goes on the wire this frame. The caller still applies it locally:
		// pacing throttles TRANSMISSION, never prediction.
		bool send = false;
		std::uint32_t sequence = 0;
	};

	// One outbound stream per owned entity: the last payload actually transmitted,
	// when the next paced repeat is due, and the sequence counter.
	//
	// The rule is "send when it CHANGED, or when the repeat is due". Change-triggered
	// is what makes an edge (a jump press held for exactly one frame) reach the host
	// at all - a pure rate limiter would sample the stream and could drop the one
	// frame the press was in. The paced repeat of an UNCHANGED payload is what makes
	// the unreliable channel safe: if the "stopped moving" packet is lost, the host
	// would otherwise keep running the character until the input next changed.
	class InputSendPacer
	{
	public:
		[[nodiscard]] InputSend Prepare(std::uint32_t netId, std::span<const std::byte> payload, float now,
		        float rateHz);

		void Forget(std::uint32_t netId);
		void Clear();

	private:
		struct Stream
		{
			std::vector<std::byte> lastSent;
			float nextRepeatTime = 0.f;
			std::uint32_t sequence = 0;
			bool everSent = false;
		};

		std::unordered_map<std::uint32_t, Stream> m_streams;
	};

	// Where one input submission goes. Pure, like RpcRoute, so the authority decision
	// is testable with no transport.
	struct InputRoute
	{
		// False = refuse outright, exactly as RouteRpc does. The one refusal is a
		// client submitting input for an entity it does not own: the host would drop
		// it, and running it locally would predict a body this peer is not allowed to
		// drive (and which SyncSimulationAuthority has already made kinematic).
		bool allowed = false;
		// Prediction. TRUE ON EVERY ALLOWED ROUTE, including the client one: the owner
		// applies its own input immediately and without waiting for the round trip,
		// which is the whole point of client-side prediction. The host is the server,
		// so its own submission is only ever local.
		bool invokeLocally = false;
		// Whether this peer additionally puts the payload on the wire.
		bool send = false;
	};

	// Offline / host  -> local only (this peer IS the authority).
	// Client, owned    -> local AND sent to the host, provided the entity has a net
	//                     id; before the spawn lands there is nothing to address, so
	//                     prediction runs and the send waits.
	// Client, not owned-> refused.
	[[nodiscard]] InputRoute RouteInput(const World& world, const NetSession& session, Entity entity);

	// ── Receive side ────────────────────────────────────────────────────────────

	// Host-side "is this the newest input I have seen for this entity". Keyed by net
	// id, which is sufficient because only the owner may drive an entity and the
	// ownership gate runs FIRST (see ApplyInput) - otherwise any client could poison
	// another player's stream by sending one packet with a huge sequence.
	class InputSequenceGate
	{
	public:
		[[nodiscard]] bool Accept(std::uint32_t netId, std::uint32_t sequence);

		void Forget(std::uint32_t netId);
		void Clear();

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> m_lastApplied;
	};

	// Host-only. Ownership gate, then staleness gate, then the RPC dispatch verbatim.
	//
	// ORDER IS LOAD-BEARING. The ownership check must precede the sequence gate: the
	// gate is keyed by net id alone, so admitting an unowned packet far enough to
	// record its sequence would let one hostile packet freeze the real owner's input
	// stream for the rest of the session.
	void ApplyInput(World& world, NetSession& session, const RpcBridge& bridge, const InputMessage& msg,
	        ConnectionId sender, InputSequenceGate& gate);
} // namespace aether::net
