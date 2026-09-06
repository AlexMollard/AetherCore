#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	// Every packet starts with one of these so the receive system can dispatch
	// without a second framing layer (NetworkReceiveSystem::OnData reads data[0] and
	// hands data.subspan(1) to the decoder).
	//
	// WHO WRITES THE KIND BYTE is not uniform, and it is load-bearing - adding a
	// kind without matching one of these two conventions produces a packet the
	// receiver misparses with no error:
	//   SELF-FRAMING (the encoder writes it): Spawn, Despawn, Rpc, Welcome, Relevancy,
	//     Disconnect, ClientReady, OwnershipRequest, OwnershipTransfer. Their encoders
	//     lead with w.U8(kind), so the sender passes the result straight to
	//     Send/Broadcast.
	//   WRAPPED (the sender writes it): Snapshot, ScriptFields, RagdollPose,
	//     VelocitySnapshot. BuildSnapshot, BuildScriptFieldPacket, BuildRagdollPoseSnapshot
	//     and BuildVelocitySnapshot all emit a bare body, which the sender must pass
	//     through FrameMessage below.
	//
	// Snapshot, ScriptFields, RagdollPose and VelocitySnapshot all travel in BOTH
	// directions: this framework is client-authoritative, so the peer that OWNS an
	// entity replicates it and the host relays. Which direction a kind is legal in
	// is therefore no longer a property of the kind alone - see the ownership gate
	// in NetSnapshot.hpp (StateWriteGate), which every one of these four reuses,
	// and is what makes the inbound half safe on the host.
	enum class NetMessage : std::uint8_t
	{
		Snapshot = 1,
		Spawn = 2,
		Despawn = 3,
		Rpc = 4,
		Welcome = 5,
		ScriptFields = 6,
		Relevancy = 7,
		Disconnect = 8,
		ClientReady = 9,
		OwnershipRequest = 10,
		OwnershipTransfer = 11,
		RagdollPose = 12,
		VelocitySnapshot = 13,
	};

	// The largest value the enum defines - the direct mirror of kNetRpcTargetMax in
	// NetRpc.hpp. Kinds are numbered contiguously from 1, so this is also how many
	// there are.
	//
	// It exists so the framing coverage check in NetFramingTests can be a
	// static_assert instead of a hand-maintained count. That test claims a kind added
	// without being classified "fails here rather than silently going untested", and
	// against a hand-written array that claim was FALSE - kind 7 was added and the
	// array stayed at 6, so Relevancy's framing went untested exactly as the comment
	// promised it could not. Anchored to the enum, adding kind 8 without a row breaks
	// the BUILD, which is the only version of that promise worth making - and so does
	// REMOVING one, which is how the retired Input kind was caught. Keep this on the
	// last enumerator.
	inline constexpr std::uint8_t kNetMessageMax = static_cast<std::uint8_t>(NetMessage::VelocitySnapshot);

	// The longest reason string a peer is allowed to put on the wire, and the longest
	// one this peer will keep. A reason is rendered by the game, so an unbounded
	// string from a remote peer is both a memory question and a layout one.
	inline constexpr std::size_t kMaxDisconnectReasonLength = 128;

	// Prefixes `payload` with its NetMessage byte - the WRAPPED half of the
	// convention above. NetworkContext::Frame is a thin forwarder to this, so both
	// the shipping path and the tests exercise the same bytes.
	[[nodiscard]] std::vector<std::byte> FrameMessage(NetMessage kind, std::span<const std::byte> payload);

	// A prefab name arrives from a remote peer and is used to open a file, so it is
	// the framework's one path-traversal boundary: anything that could name a
	// directory, a parent, or a drive is rejected before it reaches the loader. A
	// safe name is a bare, non-empty prefab identifier - no "..", no '/' or '\\',
	// no ':'. Lives here, next to the spawn message it validates, rather than
	// file-local in NetworkContext.cpp: a security check with no test is a check
	// nobody can be sure still works.
	[[nodiscard]] bool IsSafePrefabName(std::string_view name);

	struct SpawnMessage
	{
		std::uint32_t netId = 0;
		ConnectionId owner = kInvalidConnection;
		std::string prefab;
		glm::vec3 position{0.f};
	};

	[[nodiscard]] std::vector<std::byte> EncodeSpawn(std::uint32_t netId, ConnectionId owner, std::string_view prefab,
	        glm::vec3 position);
	[[nodiscard]] std::optional<SpawnMessage> DecodeSpawn(ByteReader& r);

	[[nodiscard]] std::vector<std::byte> EncodeDespawn(std::uint32_t netId);
	[[nodiscard]] std::optional<std::uint32_t> DecodeDespawn(ByteReader& r);

	// A connection-scoped "you can stop caring about this" notice: the entity is
	// still alive on the host, it has simply left THIS connection's relevancy
	// radius (another connection may still be receiving it normally). Wire-identical
	// to Despawn - one netId - but deliberately a DISTINCT kind rather than a reuse
	// of NetMessage::Despawn. See the design note on NetworkContext::ApplyRelevancyLeave
	// for why: the client reacts to both the same way today, but "the host destroyed
	// this" and "this walked out of range" are different facts, and a reused kind
	// would make them permanently indistinguishable on the wire.
	[[nodiscard]] std::vector<std::byte> EncodeRelevancyLeave(std::uint32_t netId);
	[[nodiscard]] std::optional<std::uint32_t> DecodeRelevancyLeave(ByteReader& r);

	// "This link is ending, and here is why." The DELIBERATE end of a link, which is
	// the whole reason it exists: an ENet disconnect on its own cannot tell a refusal
	// or a host quitting apart from a cable being pulled, and those want opposite
	// reactions from the game (say so and stop, versus try to come back). A link that
	// dies without one of these is by definition unexpected.
	//
	// Sent by the host in two situations - refusing a connection over the player cap,
	// and closing a session down - and never by a client: a client leaving simply
	// disconnects, and a client cannot end the host's session.
	//
	// The reason is REMOTE INPUT and is sanitised on arrival (see SanitizeReason): it
	// reaches a font and a UI rect, so a peer must not be able to send control
	// characters or an unbounded string through it.
	[[nodiscard]] std::vector<std::byte> EncodeDisconnect(std::string_view reason);
	[[nodiscard]] std::optional<std::string> DecodeDisconnect(ByteReader& r);

	// "I am standing in the scene this session's entities belong to, and I am holding
	// nothing from the one I was standing in before - send me the world."
	//
	// Client to host, and the framework's answer to a join that completes while the
	// player is still on a menu. Until it arrives the host has no way to know that the
	// peer it is replicating to is throwing every Spawn away, because a client only
	// applies a spawn into the scene it happens to be in; the entities are built into
	// the menu and destroyed with it on the scene change, and the host - which
	// remembers what it has already sent per connection - never offers them again.
	//
	// Sent only by a client whose game DECLARED that it was not ready (see
	// NetworkContext::SetReplicationReady). A client that never says otherwise is ready
	// from the moment it connects, sends none of these, and is replicated to exactly as
	// it always was - the join replay in OnConnected is untouched.
	//
	// Carries no payload: the fact IS the message, and "which scene" is deliberately
	// not on the wire. The framework has no notion of scene identity and inventing one
	// would put every project's scene naming into the protocol; the peer that knows is
	// the one standing in it.
	[[nodiscard]] std::vector<std::byte> EncodeClientReady();

	// Printable ASCII only, collapsed to at most kMaxDisconnectReasonLength characters.
	// Applied to whatever a peer sent before anything stores or shows it.
	[[nodiscard]] std::string SanitizeReason(std::string_view raw);

	// Gives every scene-placed NetworkIdentity a deterministic id, drawn from the
	// scene-placed space below kSpawnNetIdBase (NetSession::AllocateSceneNetId) so
	// it can never collide with a spawned id on any peer. Iterates in order of
	// SceneNodeComponent::id - the stable, persisted scene-node id that round-trips
	// through the scene file - which is identical on every machine loading the same
	// scene, so host and client agree with no handshake. Entities with nodeId == 0
	// (prefab-instantiated, not top-level scene entities) are skipped; they get
	// their id through spawn replication instead. ECS iteration order is NOT
	// deterministic and must never be used here.
	void AssignScenePlacedNetIds(World& world, NetSession& session);
} // namespace aether::net
