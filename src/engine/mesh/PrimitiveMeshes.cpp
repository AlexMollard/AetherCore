#include "mesh/PrimitiveMeshes.hpp"

#include <cstdint>
#include <vector>

#include "mesh/MeshGen.hpp"

namespace aether
{
	void PrimitiveMeshes::Initialize(gpu::UploadContext& uploadContext)
	{
		// -----------------------------------------------------------------------
		// Triangle  (CCW, facing +Z)
		// -----------------------------------------------------------------------
		constexpr Mesh::Vertex kTriangleVerts[] = {
		        {.position = {0.0f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.5f, 0.0f}, .color = {1.0f, 0.0f, 0.0f}},
		        {.position = {0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f}},
		        {.position = {-0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.0f, 0.0f, 1.0f}},
		};
		constexpr std::uint32_t kTriangleIndices[] = {0, 1, 2};

		// -----------------------------------------------------------------------
		// Quad  (CCW, facing +Z)  - 4 unique verts, 2 triangles
		// -----------------------------------------------------------------------
		constexpr Mesh::Vertex kQuadVerts[] = {
		        {.position = {-0.5f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {0.5f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {-0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}},
		};
		constexpr std::uint32_t kQuadIndices[] = {0, 1, 2, 0, 2, 3};

		// -----------------------------------------------------------------------
		// Cube  - 24 unique verts (4 per face), 36 indices (2 tris per face x 6)
		// Each face has its own normal and tangent.
		// -----------------------------------------------------------------------
		constexpr Mesh::Vertex kCubeVerts[] = {
		        // +Z  normal=(0,0,1)  tangent=(1,0,0,1)
		        {.position = {-0.5f, -0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {0.5f, -0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {0.5f, 0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {-0.5f, 0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        // -Z  normal=(0,0,-1)  tangent=(-1,0,0,1)
		        {.position = {0.5f, -0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {-0.5f, -0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {0.5f, 0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        // +X  normal=(1,0,0)  tangent=(0,0,-1,1)
		        {.position = {0.5f, -0.5f, 0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {0.5f, -0.5f, -0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {0.5f, 0.5f, -0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        // -X  normal=(-1,0,0)  tangent=(0,0,1,1)
		        {.position = {-0.5f, -0.5f, -0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, -0.5f, 0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, 0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, -0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        // +Y  normal=(0,1,0)  tangent=(1,0,0,1)
		        {.position = {-0.5f, 0.5f, 0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, -0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        {.position = {-0.5f, 0.5f, -0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        // -Y  normal=(0,-1,0)  tangent=(1,0,0,1)
		        {.position = {-0.5f, -0.5f, -0.5f}, .normal = {0.0f, -1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 0.2f, 1.0f}},
		        {.position = {0.5f, -0.5f, -0.5f}, .normal = {0.0f, -1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 0.2f, 1.0f}},
		        {.position = {0.5f, -0.5f, 0.5f}, .normal = {0.0f, -1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 0.2f, 1.0f}},
		        {.position = {-0.5f, -0.5f, 0.5f}, .normal = {0.0f, -1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 0.2f, 1.0f}},
		};
		constexpr std::uint32_t kCubeIndices[] = {
		        0,
		        1,
		        2,
		        0,
		        2,
		        3, // +Z
		        4,
		        5,
		        6,
		        4,
		        6,
		        7, // -Z
		        8,
		        9,
		        10,
		        8,
		        10,
		        11, // +X
		        12,
		        13,
		        14,
		        12,
		        14,
		        15, // -X
		        16,
		        17,
		        18,
		        16,
		        18,
		        19, // +Y
		        20,
		        21,
		        22,
		        20,
		        22,
		        23, // -Y
		};

		m_triangle = Mesh::Create(uploadContext, kTriangleVerts, kTriangleIndices);
		m_quad = Mesh::Create(uploadContext, kQuadVerts, kQuadIndices);
		m_cube = Mesh::Create(uploadContext, kCubeVerts, kCubeIndices);

		// -----------------------------------------------------------------------
		// Plane  - default 20x20 subdivided grid via MeshGen.
		// UVs tile 20x per axis (1 UV unit per segment).
		// -----------------------------------------------------------------------
		{
			const MeshGen::MeshData plane = MeshGen::GeneratePlane({.segmentsX = 20, .segmentsY = 20, .uvScale = 1.0f});
			m_plane = Mesh::Create(uploadContext, std::span<const Mesh::Vertex>(plane.vertices), std::span<const std::uint32_t>(plane.indices));
		}

		{
			const MeshGen::MeshData sphere = MeshGen::GenerateUVSphere({.stacks = 16, .slices = 32});
			m_sphere = Mesh::Create(uploadContext, std::span<const Mesh::Vertex>(sphere.vertices), std::span<const std::uint32_t>(sphere.indices));
		}
	}

	void PrimitiveMeshes::Destroy()
	{
		m_triangle.Destroy();
		m_quad.Destroy();
		m_cube.Destroy();
		m_plane.Destroy();
		m_sphere.Destroy();
	}

	const Mesh& PrimitiveMeshes::Get(PrimitiveMesh primitive) const
	{
		switch (primitive)
		{
			case PrimitiveMesh::Triangle:
				return m_triangle;
			case PrimitiveMesh::Quad:
				return m_quad;
			case PrimitiveMesh::Cube:
				return m_cube;
			case PrimitiveMesh::Plane:
				return m_plane;
			case PrimitiveMesh::Sphere:
				return m_sphere;
			default:
				return m_triangle;
		}
	}
} // namespace aether
