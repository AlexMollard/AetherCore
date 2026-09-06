#pragma once

#include <cstddef>
#include <optional>
#include <vector>
#include <glm/glm.hpp>

namespace aether::assets
{
	struct GltfAsset;
}

namespace aether
{
	// A hostile or absurdly large mesh must not stall physics body creation or make a
	// hull/mesh shape unreasonably expensive to build. This is independent of Jolt's own
	// hard 256-point ceiling on a HULL'S RESULT (JPH::ConvexHullShape::cMaxPointsInHull) -
	// Jolt happily reduces thousands of INPUT points down to a 256-vertex hull (interior
	// points are discarded during construction), so that ceiling alone does not bound the
	// cost of feeding it an unreasonably large source mesh. This is a defensive cap on the
	// INPUT, in the same spirit as GltfAsset.cpp's own kMaxMeshVertices ceiling (which is
	// far larger - that one guards a corrupt header from claiming an absurd allocation;
	// this one guards a legitimate but oversized source mesh from making every collider
	// load slow).
	constexpr std::size_t kMaxColliderMeshVertices = 4096;

	// Every vertex position across every primitive, concatenated, plus every triangle as
	// flat index triples into that concatenated list (already bounds-validated against
	// each primitive's own vertex count - an out-of-range index is skipped, never used).
	struct ColliderMeshGeometry
	{
		std::vector<glm::vec3> positions;
		std::vector<std::uint32_t> indices; // flat triples: indices[3*i + 0..2] is one triangle
	};

	// Gathers a ConvexHull/Mesh collider's source geometry from an already-loaded glTF/
	// mesh asset. Pure and synchronous - no file I/O here; the caller loads `asset`
	// (assets::GltfAsset::LoadFromVfsPath for a real collider, or a hand-built GltfAsset in
	// a test, exactly like RagdollBuilder::SpawnRagdoll takes its skeleton). Returns
	// nullopt if the asset has no vertices at all, or if the total vertex count exceeds
	// kMaxColliderMeshVertices - the caller decides what to log; this function stays
	// silent so it is equally usable from a test.
	[[nodiscard]] std::optional<ColliderMeshGeometry> BuildColliderMeshGeometry(const assets::GltfAsset& asset);
} // namespace aether
