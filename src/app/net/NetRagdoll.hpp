#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "net/NetSerialize.hpp"
#include "net/NetSnapshot.hpp" // StateWriteGate
#include "net/NetSpawn.hpp" // NetMessage
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	class NetSession;

	// One bone's pose: world-space position and rotation (Euler degrees, the same
	// representation TransformSample and every other replicated transform in this
	// framework already use). Deliberately WORLD space rather than local-to-parent
	// - a ragdoll's actual joint topology (which bone parents which, at what
	// anchor) is private to RagdollBuilder.cpp's anonymous namespace and exposed
	// nowhere this layer can reach, so there is no "local to parent" representation
	// available to compute even if a smaller one were wanted. See NetRagdoll.cpp's
	// own comment on BuildRagdollPoseSnapshot for the bandwidth this costs and why
	// it was accepted anyway.
	struct RagdollBonePose
	{
		glm::vec3 position{0.f};
		glm::vec3 rotation{0.f};
	};

	// One ragdoll's worth of bone poses, addressed by its ROOT's net id - the only
	// net id a ragdoll has at all (see RagdollBuilder.hpp: the root keeps the
	// source entity's existing NetworkIdentity verbatim; every other bone carries
	// none). `bones` is every entry of RagdollComponent::bones EXCEPT the root
	// itself, in that vector's own order - see BuildRagdollPoseSnapshot/
	// ApplyRagdollPoses for why relying on that order, rather than a per-bone id,
	// is safe here and nowhere else in this framework.
	struct RagdollPoseEntry
	{
		std::uint32_t netId = 0;
		std::vector<RagdollBonePose> bones;
	};

	[[nodiscard]] std::vector<std::byte> EncodeRagdollPoses(const std::vector<RagdollPoseEntry>& entries);
	[[nodiscard]] std::optional<std::vector<RagdollPoseEntry>> DecodeRagdollPoses(ByteReader& r);

	// Sender side. `candidates` is exactly the entity list Snapshot itself builds
	// from (NetworkSendSystem's `replicated`/`owned`) - every entity already
	// filtered to what THIS peer should tell this connection (or the host) about,
	// so a ragdoll's pose is admitted, relayed and forgotten on exactly the same
	// relevancy/ownership terms as everything else. An entity with no
	// RagdollComponent, no net id yet, or a bone that has somehow lost its
	// TransformComponent contributes nothing (the last case drops that WHOLE
	// ragdoll's entry for this tick rather than risk a bone-index shift landing
	// pose data on the wrong limb - see the .cpp). Returns an empty vector when no
	// candidate qualifies - callers must not send an empty packet, exactly like
	// BuildSnapshot.
	[[nodiscard]] std::vector<std::byte> BuildRagdollPoseSnapshot(World& world, const std::vector<Entity>& candidates);

	// Receiver side. Resolves each entry's netId to a local entity, refuses one the
	// gate does not allow (the same StateWriteGate Snapshot itself is applied
	// through) or that carries no RagdollComponent, and writes every OTHER bone's
	// TransformComponent directly. NO SMOOTHING: unlike a NetworkIdentity-bearing
	// entity, a ragdoll bone has no InterpolationBuffer of its own to ease into -
	// see the .cpp's own note on why that is a deliberate, named v1 boundary and
	// not an oversight. A bone index past what THIS peer's own RagdollComponent
	// has is dropped rather than applied out of bounds - defensive tolerance for a
	// version-skewed peer, not something this framework's own two ends should ever
	// actually disagree on (see RagdollPoseEntry's own comment on why the order is
	// otherwise trusted).
	void ApplyRagdollPoses(World& world, const NetSession& session, std::span<const std::byte> payload,
	        const StateWriteGate& gate);
} // namespace aether::net
