#pragma once

#include <cstdint>
#include <string>

#include "net/NetTypes.hpp"
#include "physics2d/Physics2DComponents.hpp"

namespace aether::net
{
	// Marks an entity as replicated and carries its network identity. `netId` is
	// host-assigned and stable for the entity's lifetime; `owner` is the connection
	// allowed to drive it (0 = the host owns it).
	struct NetworkIdentity
	{
		std::uint32_t netId = 0;
		ConnectionId owner = kInvalidConnection;
		// Prefab this entity was spawned from, so a joining client can recreate it.
		// Empty for scene-placed entities, which both ends already have.
		std::string spawnPrefab;
		bool scenePlaced = false;
	};

	// Smoothing for a replicated entity's transform, and it applies to REMOTE
	// entities only: their buffer is rendered `interpolationDelaySeconds` in the
	// past, so there is a sample on both sides of the render time to interpolate
	// between and motion is smooth between packets.
	//
	// There is deliberately nothing here for the locally-owned entity. The owner of
	// an entity is authoritative for it, so its own simulation IS the truth and
	// receives no correction of any kind - see NetworkReceiveSystem::ResolveTransforms.
	// The correction-rate and snap-distance knobs that used to live here belonged to
	// the host-authoritative model and were removed with it: a tuning field nothing
	// reads is worse than none.
	struct NetworkTransform
	{
		float interpolationDelaySeconds = 0.1f;
	};

	// A connected player's display name and link quality. Framework-level rather than
	// game-level: name tags, chat attribution and disconnect notices all read this one
	// field instead of each tracking names separately.
	//
	// Both fields are REPLICATED and both are AUTHORED BY THE OWNER, which is the only
	// arrangement that works under client authority - a peer writing either of these on
	// somebody else's player is writing a value that player's owner overwrites on its
	// next send. `displayName` learned that the hard way (see Net.SetPlayerName's
	// refusal), and `pingMs` is the same shape of state: only the peer at one end of a
	// link knows what that link costs, so only it may say.
	struct NetPlayer
	{
		std::string displayName;

		// Round-trip time to the HOST, in milliseconds, as measured by this player's
		// own peer. 0 on the host's own player, which has no link to itself, and 0
		// offline - both of which read correctly as "no latency to speak of".
		//
		// Stamped by NetworkSendSystem on the entities this peer owns, so a game gets a
		// per-player ping on every peer with no code of its own and no second message
		// on the wire. Replication carries it outward like any other owned field.
		std::uint32_t pingMs = 0;
	};

	// Present on a replicated entity whose 2D body this peer has taken OFF local
	// simulation because it is not authoritative for it - see
	// NetworkContext::SyncSimulationAuthority. Runtime-only and deliberately
	// unreflected: it is never authored, never serialized, and never replicated.
	//
	// It carries the authored body type so the switch is reversible, and its mere
	// PRESENCE is what makes the switch idempotent: an entity already carrying one
	// is already handed over, so the reconcile pass leaves it alone instead of
	// rebuilding its body every frame.
	struct NetSimulationOverride
	{
		Body2DType authoredBodyType = Body2DType::Dynamic;
	};
} // namespace aether::net
