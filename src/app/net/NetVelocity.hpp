#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetSerialize.hpp"
#include "net/NetSnapshot.hpp" // StateWriteGate
#include "net/NetSpawn.hpp" // NetMessage

namespace aether
{
	class World;
}

namespace aether::net
{
	class NetSession;
	class NetworkContext;

	// One entity's current linear/angular velocity, read directly from its owner's
	// physics body rather than derived from position history. See NetVelocity.cpp's
	// own comment on BuildVelocitySnapshot for exactly which entities qualify and
	// why that set was chosen over "every replicated entity" or "an authored opt-in
	// flag" - both closed to this feature by the same reflection boundary
	// NetOwnership.hpp and NetRagdoll.hpp already work around.
	struct VelocityEntry
	{
		std::uint32_t netId = 0;
		glm::vec3 linear{0.f};
		glm::vec3 angular{0.f};
	};

	[[nodiscard]] std::vector<std::byte> EncodeVelocitySnapshot(const std::vector<VelocityEntry>& entries);
	[[nodiscard]] std::optional<std::vector<VelocityEntry>> DecodeVelocitySnapshot(ByteReader& r);

	// Sender side. `candidates` is exactly the entity list Snapshot itself builds
	// from (NetworkSendSystem's `replicated`/`owned`), so a velocity entry is
	// admitted, relayed and forgotten on exactly the same relevancy/ownership
	// terms as everything else already sent this tick.
	//
	// An entity qualifies only when THIS peer is its actual owner (context.
	// OwnsIdentity) AND it carries a RigidBodyComponent whose motionType is
	// Dynamic - "physics props yes, a slow-moving (Kinematic, or bodyless) door
	// no", read straight off the component composition the entity already has
	// rather than a new authored flag this feature cannot add (see the .cpp).
	//
	// A candidate this peer does NOT own (the host relaying another connection's
	// entity) reports whatever NetReceivedVelocity last told this peer about it,
	// if anything - see that component's own comment for why a live physics query
	// would lie in that case. Nothing to relay yet contributes nothing, exactly
	// like a field Snapshot has never received a value for.
	[[nodiscard]] std::vector<std::byte> BuildVelocitySnapshot(World& world, const NetworkContext& context,
	        const std::vector<Entity>& candidates);

	// Receiver side. Resolves each entry to a local entity, refuses one the gate
	// does not allow (the same StateWriteGate Snapshot itself is applied through),
	// and writes NetReceivedVelocity onto it so a HOST can relay the value onward
	// even though it does not own the entity - see that component's own comment.
	// Returns the same (netId, linear, angular) the gate accepted, so the caller
	// can ALSO feed it into the per-entity interpolation state
	// (NetworkReceiveSystem::m_remote) this file has no access to and must not
	// reach into.
	[[nodiscard]] std::vector<VelocityEntry> ApplyVelocitySnapshot(World& world, const NetSession& session,
	        std::span<const std::byte> payload, const StateWriteGate& gate);
} // namespace aether::net
