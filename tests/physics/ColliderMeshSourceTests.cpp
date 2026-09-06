// Coverage for BuildColliderMeshGeometry, the asset-consuming half of ConvexHull/Mesh
// collider support: gathering vertex positions and bounds-validated triangle indices
// from an already-loaded glTF/mesh asset, with a defensive vertex-count cap. This is
// the file-I/O-free half (mirrors RagdollBuilder's own split: SpawnRagdoll takes an
// already-loaded GltfAsset, never touches disk itself), so these tests build a
// synthetic GltfAsset by hand - no real .mesh/.glb fixture needed, and no dependency
// on tools/assetpack's baking to have run.
#include <doctest/doctest.h>

#include "assets/GltfAsset.hpp"
#include "physics/ColliderMeshSource.hpp"

using namespace aether;

namespace
{
	assets::GltfPrimitive MakePrimitive(std::initializer_list<glm::vec3> positions, std::initializer_list<std::uint32_t> indices)
	{
		assets::GltfPrimitive prim;
		for (const glm::vec3& p: positions)
		{
			Mesh::Vertex v{};
			v.position = p;
			prim.vertices.push_back(v);
		}
		prim.indices = indices;
		return prim;
	}
} // namespace

TEST_CASE("BuildColliderMeshGeometry gathers every primitive's vertices, concatenated and index-offset")
{
	assets::GltfAsset asset;
	asset.primitives.push_back(MakePrimitive({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, {0, 1, 2}));
	asset.primitives.push_back(MakePrimitive({{2.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.0f}}, {0, 1, 2}));

	const auto geo = BuildColliderMeshGeometry(asset);
	REQUIRE(geo.has_value());
	CHECK(geo->positions.size() == 6);
	// Second primitive's own local indices (0,1,2) must be offset by the first
	// primitive's vertex count (3) once concatenated - a naive un-offset concat would
	// point every triangle back at the first primitive's own three vertices.
	REQUIRE(geo->indices.size() == 6);
	CHECK(geo->indices[3] == 3);
	CHECK(geo->indices[4] == 4);
	CHECK(geo->indices[5] == 5);
	CHECK(geo->positions[3] == glm::vec3{2.0f, 0.0f, 0.0f});
}

TEST_CASE("BuildColliderMeshGeometry skips a triangle naming an out-of-range local index rather than reading past the vertex list")
{
	assets::GltfAsset asset;
	// Index 5 does not exist (only 3 vertices, 0-2) - a hand-corrupted or malformed
	// mesh file could produce exactly this, and it must be dropped, not used to index
	// past geo.positions.
	asset.primitives.push_back(MakePrimitive({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, {0, 1, 5}));

	const auto geo = BuildColliderMeshGeometry(asset);
	REQUIRE(geo.has_value());
	CHECK(geo->positions.size() == 3);
	CHECK(geo->indices.empty());
}

TEST_CASE("BuildColliderMeshGeometry refuses an asset with no vertices at all")
{
	assets::GltfAsset asset; // no primitives
	CHECK_FALSE(BuildColliderMeshGeometry(asset).has_value());

	assets::GltfAsset withEmptyPrimitive;
	withEmptyPrimitive.primitives.push_back(assets::GltfPrimitive{});
	CHECK_FALSE(BuildColliderMeshGeometry(withEmptyPrimitive).has_value());
}

TEST_CASE("BuildColliderMeshGeometry refuses a mesh over the collider vertex cap")
{
	// A legitimate but oversized source mesh (not a corrupt one) must still be
	// refused - kMaxColliderMeshVertices exists to bound the COST of feeding a large
	// point set into hull/mesh construction, independent of whether the data is valid.
	assets::GltfAsset asset;
	assets::GltfPrimitive prim;
	prim.vertices.resize(kMaxColliderMeshVertices + 1);
	asset.primitives.push_back(std::move(prim));

	CHECK_FALSE(BuildColliderMeshGeometry(asset).has_value());

	// Exactly at the cap must still succeed - this is an off-by-one boundary, the
	// kind of thing a "> vs >=" typo would silently get wrong in either direction.
	assets::GltfAsset atCap;
	assets::GltfPrimitive primAtCap;
	primAtCap.vertices.resize(kMaxColliderMeshVertices);
	atCap.primitives.push_back(std::move(primAtCap));
	CHECK(BuildColliderMeshGeometry(atCap).has_value());
}
