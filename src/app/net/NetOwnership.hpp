#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "net/NetSerialize.hpp"
#include "net/NetSpawn.hpp" // NetMessage
#include "net/NetTypes.hpp"

namespace aether::net
{
	// One entity changing hands: {netId, newOwner}. Carried by BOTH ownership
	// messages below - a request and the authoritative result look identical on
	// the wire, which is why one struct serves DecodeOwnershipRequest and
	// DecodeOwnershipTransfer alike.
	struct OwnershipTransferMessage
	{
		std::uint32_t netId = 0;
		ConnectionId newOwner = kInvalidConnection;
	};

	// "I would like `netId` to belong to `newOwner`." CLIENT TO HOST ONLY - the host
	// is the only peer that ever decides who owns what (see ValidateOwnershipRequest
	// below), so this is a SECOND channel, beside Rpc's Server target, by which a
	// client may affect host state at all. Unlike an Rpc it needs no script and no
	// entity the caller already owns to carry it: a physics gun grabbing a prop it
	// does NOT yet own has nothing of its own to attach a [NetRpc(Server)] call to
	// (see the RPC ownership gate in NetRpc.hpp) - which is exactly the gap this
	// message exists to close.
	//
	// A refused request gets no reply - see ValidateOwnershipRequest's comment for
	// why a silent drop, matching every other inbound refusal this framework makes,
	// is the right answer here too.
	[[nodiscard]] std::vector<std::byte> EncodeOwnershipRequest(std::uint32_t netId, ConnectionId newOwner);
	[[nodiscard]] std::optional<OwnershipTransferMessage> DecodeOwnershipRequest(ByteReader& r);

	// "`netId` now belongs to `newOwner` - settled." HOST TO EVERYONE (broadcast),
	// the authoritative result of either a granted OwnershipRequest or a direct,
	// host-side NetworkContext::RequestOwnershipTransfer call. A DISTINCT kind from
	// OwnershipRequest even though the payload shape is identical, for the same
	// reason Relevancy is distinct from Despawn: "please give me this" and "this is
	// now decided" are different facts, and a client must never apply the first as
	// if it were the second - see the direction gate in
	// NetworkReceiveSystem::OnData.
	[[nodiscard]] std::vector<std::byte> EncodeOwnershipTransfer(std::uint32_t netId, ConnectionId newOwner);
	[[nodiscard]] std::optional<OwnershipTransferMessage> DecodeOwnershipTransfer(ByteReader& r);

	// What NetworkContext::RequestOwnershipTransfer actually did, so a caller (and
	// Net.RequestOwnership/Net.ReleaseOwnership) can tell "granted immediately" from
	// "asked, and still waiting on the host" without polling NetworkIdentity::owner
	// itself.
	enum class OwnershipTransferOutcome : std::uint8_t
	{
		// Offline, or on the host: the owner field changed right here, on this call.
		// A host also broadcasts it; offline there is nobody to tell.
		Applied,
		// On a client: sent to the host, awaiting its NetMessage::OwnershipTransfer.
		// NetworkIdentity::owner is unchanged until that arrives.
		Requested,
		// The entity handle passed in was invalid or already destroyed. Never
		// returned for a legitimate client request that the host later refuses - a
		// client is never told why the host said no, only that its owner field
		// never changed (the same silence the RPC ownership gate already models).
		Refused,
	};

	// Why an inbound NetMessage::OwnershipRequest is refused, or None to grant it.
	// Pure and testable with no World/NetworkContext involved:
	// NetworkReceiveSystem::OnData resolves `currentOwner` from the target entity's
	// NetworkIdentity and calls straight through - the same shape as NetRpc.hpp's
	// RouteRpc/RpcTargetMismatch.
	//
	// THE RULE: "the host decides; a client may only ask for itself, and only for
	// what is currently free or already its own." Concretely -
	//   - A client may name itself or the host (kInvalidConnection, "give it back")
	//     as `newOwner`, never a THIRD connection - reassigning somebody else's
	//     property without that owner's own say-so is refused outright, whatever the
	//     sender claims.
	//   - Claiming (newOwner == sender): allowed when the entity is currently
	//     unowned (owner == host) or already the sender's own (idempotent - a
	//     physics gun re-asserting its grip every frame must not be refused).
	//     Claiming an entity another CONNECTED peer currently holds is refused -
	//     that peer must release it (or disconnect) first. This is
	//     "currently-unowned-or-mine", the option this framework picked over a
	//     proximity check: proximity needs a notion of "the requester's own
	//     entity" that this engine-level message has no way to know (see
	//     docs/multiplayer.md's RPC rule for where that idea DOES work - attached
	//     to the caller's own entity, which this message is not).
	//   - Releasing (newOwner == host): allowed only when the sender IS the
	//     CURRENT owner - a stranger cannot release a prop it never held.
	//
	// HOST-ORIGINATED CALLS SKIP THIS ENTIRELY (see
	// NetworkContext::RequestOwnershipTransfer) - "host always wins" is the other
	// half of the rule: the host is this session's authority for every other
	// mutation (Spawn, Despawn, a disconnect's release), and ownership is no
	// different. A host-side game mode may hand any entity to any connection, or
	// reclaim one, with no round trip and no gate.
	enum class OwnershipTransferRefusal : std::uint8_t
	{
		None,
		TargetNotSelfOrHost, // newOwner named a connection other than the sender or the host
		NotFreeOrOwn,        // claim: currentOwner is a DIFFERENT, still-connected peer
		NotCurrentOwner,     // release: sender does not currently own the entity
	};

	[[nodiscard]] OwnershipTransferRefusal ValidateOwnershipRequest(ConnectionId currentOwner, ConnectionId sender,
	        ConnectionId newOwner);
} // namespace aether::net
