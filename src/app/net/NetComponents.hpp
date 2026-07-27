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

	// Smoothing for a replicated entity's transform. On remote entities the buffer
	// is rendered `interpolationDelaySeconds` in the past so motion is smooth
	// between packets; on the locally-owned (predicted) entity the authoritative
	// position is eased in at `correctionRate` instead of snapping - unless the
	// error exceeds `snapDistance`, where easing would look worse than a cut.
	struct NetworkTransform
	{
		float interpolationDelaySeconds = 0.1f;
		float correctionRate = 12.f;
		float snapDistance = 4.f;
	};

	// A connected player's display name. Framework-level rather than game-level:
	// name tags, chat attribution and disconnect notices all read this one field
	// instead of each tracking names separately.
	struct NetPlayer
	{
		std::string displayName;
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
