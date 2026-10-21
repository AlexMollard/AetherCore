#include "mesh/PrimitiveMeshData.hpp"

#include <iterator>

namespace aether
{
	std::optional<MeshGen::MeshData> GeneratePrimitiveMesh(PrimitiveMesh primitive)
	{
		constexpr MeshVertex kTriangleVerts[] = {
		        {.position = {0.0f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.5f, 0.0f}, .color = {1.0f, 0.0f, 0.0f}},
		        {.position = {0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f}},
		        {.position = {-0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {0.0f, 0.0f, 1.0f}},
		};
		constexpr std::uint32_t kTriangleIndices[] = {0, 1, 2};

		constexpr MeshVertex kQuadVerts[] = {
		        {.position = {-0.5f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {0.5f, -0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 1.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {1.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}},
		        {.position = {-0.5f, 0.5f, 0.0f}, .normal = {0.0f, 0.0f, 1.0f}, .tangent = {1.0f, 0.0f, 0.0f, 1.0f}, .uv = {0.0f, 0.0f}, .color = {1.0f, 1.0f, 1.0f}},
		};
		constexpr std::uint32_t kQuadIndices[] = {0, 1, 2, 0, 2, 3};

		constexpr MeshVertex kCubeVerts[] = {
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

		switch (primitive)
		{
			case PrimitiveMesh::Triangle:
				return MeshGen::MeshData{{std::begin(kTriangleVerts), std::end(kTriangleVerts)}, {std::begin(kTriangleIndices), std::end(kTriangleIndices)}};
			case PrimitiveMesh::Quad:
				return MeshGen::MeshData{{std::begin(kQuadVerts), std::end(kQuadVerts)}, {std::begin(kQuadIndices), std::end(kQuadIndices)}};
			case PrimitiveMesh::Cube:
				return MeshGen::MeshData{{std::begin(kCubeVerts), std::end(kCubeVerts)}, {std::begin(kCubeIndices), std::end(kCubeIndices)}};
			case PrimitiveMesh::Plane:
				return MeshGen::GeneratePlane({.segmentsX = 20, .segmentsY = 20, .uvScale = 1.0f});
			case PrimitiveMesh::Sphere:
				return MeshGen::GenerateUVSphere({.stacks = 16, .slices = 32});
			default:
				return std::nullopt;
		}
	}
} // namespace aether
