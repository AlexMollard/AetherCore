#pragma once

#include <cstdint>
#include <vector>

#include "mesh/Mesh.hpp"

// -----------------------------------------------------------------------------
// MeshGen - procedural CPU-side geometry generation.
//
// Each function returns a MeshData (vertices + indices) that can be uploaded
// directly via AssetManager::CreateMesh or AetherCore::CreateMesh.
//
// All primitives are unit-sized (extent +-0.5 along each relevant axis) and
// centered at the origin.  Scale via the entity transform.
// -----------------------------------------------------------------------------

namespace aether::MeshGen
{
	// Raw CPU geometry; upload with AssetManager::CreateMesh(data.vertices, data.indices).
	struct MeshData
	{
		std::vector<Mesh::Vertex> vertices;
		std::vector<std::uint32_t> indices;
	};

	// -- Plane -----------------------------------------------------------------
	// Faces +Z.  UV origin is at the bottom-left corner of the mesh.
	//
	//   segmentsX / segmentsY  - number of quads along each axis (>= 1).
	//   uvScale                - UV units per segment.
	//                            1.0 = one texture tile per segment quad.
	//                            With REPEAT addressing the texture tiles across
	//                            the full plane segmentsX x segmentsY times.
	struct PlaneDesc
	{
		int segmentsX = 10;
		int segmentsY = 10;
		float uvScale = 1.0f;
	};

	[[nodiscard]] MeshData GeneratePlane(const PlaneDesc& desc);

	// -- UV Sphere -------------------------------------------------------------
	// Latitude/longitude sphere, radius 0.5, +Y = north pole.
	//
	//   stacks  - latitude bands (>= 2).
	//   slices  - longitude segments (>= 3).
	//   uvScale - UV scale applied to both axes (1.0 = full 0->1 wrap).
	struct UVSphereDesc
	{
		int stacks = 16;
		int slices = 32;
		float uvScale = 1.0f;
	};

	[[nodiscard]] MeshData GenerateUVSphere(const UVSphereDesc& desc);

	// -- Cylinder -------------------------------------------------------------
	// Aligned along Y.  Radius 0.5, height 1.0 (Y = -0.5 to +0.5).
	//
	//   segments      - circumference divisions (>= 3).
	//   caps          - whether to generate top and bottom disc caps.
	//   uvScaleRadial - U scale along the circumference (1.0 = 0->1 once around).
	//   uvScaleAxial  - V scale along the height    (1.0 = 0->1 bottom to top).
	struct CylinderDesc
	{
		int segments = 32;
		bool caps = true;
		float uvScaleRadial = 1.0f;
		float uvScaleAxial = 1.0f;
	};

	[[nodiscard]] MeshData GenerateCylinder(const CylinderDesc& desc);

} // namespace aether::MeshGen
