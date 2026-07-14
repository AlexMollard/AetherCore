#include "mesh/PrimitiveMeshes.hpp"

#include <cstdint>
#include <vector>

#include "mesh/MeshGen.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void PrimitiveMeshes::Initialize(gpu::UploadContext& uploadContext)
	{
		AE_PROFILE_ZONE();
		constexpr Mesh::Vertex kTriangleVerts[] = {
		        {.position = {0.0f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.5f, 0.0f}, .color = {1.0f, 0.0f, 0.0f}},
		        {.position = {0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f}},
		        {.position = {-0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.0f, 0.0f, 1.0f}},
		};
		constexpr std::uint32_t kTriangleIndices[] = {0, 1, 2};

		constexpr Mesh::Vertex kQuadVerts[] = {
		        {.position = {-0.5f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {0.5f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {-0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}},
		};
		constexpr std::uint32_t kQuadIndices[] = {0, 1, 2, 0, 2, 3};

		constexpr Mesh::Vertex kCubeVerts[] = {
		        {.position = {-0.5f, -0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {0.5f, -0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {0.5f, 0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {-0.5f, 0.5f, 0.5f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 0.2f, 0.2f}},
		        {.position = {0.5f, -0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {-0.5f, -0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {0.5f, 0.5f, -0.5f}, .normal = {0.0f, 0.0f, -1.0f}, .tangent = {-1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {0.2f, 1.0f, 0.2f}},
		        {.position = {0.5f, -0.5f, 0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {0.5f, -0.5f, -0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {0.5f, 0.5f, -0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.5f}, .normal = {1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, -1.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {0.2f, 0.2f, 1.0f}},
		        {.position = {-0.5f, -0.5f, -0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, -0.5f, 0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, 0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, -0.5f}, .normal = {-1.0f, 0.0f, 0.0f}, .tangent = {0.0f, 0.0f, 1.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 0.2f}},
		        {.position = {-0.5f, 0.5f, 0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, -0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {0.2f, 1.0f, 1.0f}},
		        {.position = {-0.5f, 0.5f, -0.5f}, .normal = {0.0f, 1.0f, 0.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {0.2f, 1.0f, 1.0f}},
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
		        3,
		        4,
		        5,
		        6,
		        4,
		        6,
		        7,
		        8,
		        9,
		        10,
		        8,
		        10,
		        11,
		        12,
		        13,
		        14,
		        12,
		        14,
		        15,
		        16,
		        17,
		        18,
		        16,
		        18,
		        19,
		        20,
		        21,
		        22,
		        20,
		        22,
		        23,
		};

		constexpr float kTriAabbMin[3] = {-0.5f, -0.5f, 0.0f};
		constexpr float kTriAabbMax[3] = {0.5f, 0.5f, 0.0f};
		constexpr float kTriSphereCenter[3] = {0.0f, 0.0f, 0.0f};
		constexpr float kTriSphereRadius = 0.70710678f;

		m_triangle = Mesh::Create(uploadContext, kTriangleVerts, kTriangleIndices, kTriAabbMin, kTriAabbMax, kTriSphereCenter, kTriSphereRadius);
		m_quad = Mesh::Create(uploadContext, kQuadVerts, kQuadIndices, kTriAabbMin, kTriAabbMax, kTriSphereCenter, kTriSphereRadius);

		constexpr float kCubeAabbMin[3] = {-0.5f, -0.5f, -0.5f};
		constexpr float kCubeAabbMax[3] = {0.5f, 0.5f, 0.5f};
		constexpr float kCubeSphereCenter[3] = {0.0f, 0.0f, 0.0f};
		constexpr float kCubeSphereRadius = 0.8660254f;

		m_cube = Mesh::Create(uploadContext, kCubeVerts, kCubeIndices, kCubeAabbMin, kCubeAabbMax, kCubeSphereCenter, kCubeSphereRadius);

		{
			const MeshGen::MeshData plane = MeshGen::GeneratePlane({.segmentsX = 20, .segmentsY = 20, .uvScale = 1.0f});
			constexpr float kPlaneAabbMin[3] = {-0.5f, -0.5f, 0.0f};
			constexpr float kPlaneAabbMax[3] = {0.5f, 0.5f, 0.0f};
			constexpr float kPlaneSphereCenter[3] = {0.0f, 0.0f, 0.0f};
			constexpr float kPlaneSphereRadius = 0.70710678f;
			m_plane = Mesh::Create(uploadContext, std::span<const Mesh::Vertex>(plane.vertices), std::span<const std::uint32_t>(plane.indices), kPlaneAabbMin, kPlaneAabbMax, kPlaneSphereCenter, kPlaneSphereRadius);
		}

		{
			const MeshGen::MeshData sphere = MeshGen::GenerateUVSphere({.stacks = 16, .slices = 32});
			constexpr float kSphereAabbMin[3] = {-0.5f, -0.5f, -0.5f};
			constexpr float kSphereAabbMax[3] = {0.5f, 0.5f, 0.5f};
			constexpr float kSphereSphereCenter[3] = {0.0f, 0.0f, 0.0f};
			constexpr float kSphereSphereRadius = 0.5f;
			m_sphere = Mesh::Create(uploadContext, std::span<const Mesh::Vertex>(sphere.vertices), std::span<const std::uint32_t>(sphere.indices), kSphereAabbMin, kSphereAabbMax, kSphereSphereCenter, kSphereSphereRadius);
		}
	}

	void PrimitiveMeshes::Destroy()
	{
		AE_PROFILE_ZONE();
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
