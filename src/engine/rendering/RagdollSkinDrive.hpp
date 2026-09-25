#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

#include "rendering/GpuContracts.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;

	// Builds this frame's ragdoll-driven override list for one skinned mesh instance:
	// one entry per driving bone (RagdollComponent::bones off `ragdollRoot`) whose
	// RagdollBoneComponent::skinNodeName resolves against targetNodeNames, each
	// carrying the bone's CURRENT physics pose reconstructed as
	// bodyWorld(t) * RagdollBoneComponent::skinNodeOffset (see that field's own
	// comment for why this is exact, not an approximation) and converted into the
	// mesh's own model space via meshWorldToModel. A bone that cannot be matched (no
	// RagdollBoneComponent, no TransformComponent, or its skinNodeName is not found in
	// targetNodeNames) is simply skipped - the mesh renders its last sampled
	// animation pose for that one joint rather than guessing.
	//
	// Takes plain node names rather than an AnimationDatabase& so the math here is
	// testable without a live Vulkan device: production code passes
	// animDb.GetNodeNames(); a unit test passes a synthetic list. Returns an empty
	// vector (not an error) if ragdollRoot carries no RagdollComponent - the caller's
	// RagdollSkinDriveComponent may simply be stale (the ragdoll it named is gone).
	[[nodiscard]] std::vector<AnimationContracts::RagdollOverrideEntry> BuildRagdollSkinOverrides(
	        const World& world, Entity ragdollRoot, std::span<const std::string> targetNodeNames, const glm::mat4& meshWorldToModel);

	// The animated node a name refers to: a case-insensitive exact match, else a match on the
	// node's local name after its last '_'/':' separator (a differently-prefixed export).
	[[nodiscard]] std::optional<std::uint32_t> FindNodeIndexByName(std::span<const std::string> nodeNames, std::string_view target);

	// Appends one kNodeOverrideLocalRotation entry per script joint offset: the node's local
	// transform is post-multiplied by the rotation in node_flatten.slang.
	void AppendJointRotationOverrides(std::span<const JointRotationOffset> offsets, std::vector<AnimationContracts::RagdollOverrideEntry>& out);
} // namespace aether
