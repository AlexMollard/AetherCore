#include "PrimitiveMeshes.hpp"

#include <cstdint>

namespace aether
{
	void PrimitiveMeshes::Initialize(VkDevice device, VmaAllocator allocator)
	{
		// -----------------------------------------------------------------------
		// Triangle  (CCW, facing +Z)
		// -----------------------------------------------------------------------
		constexpr Mesh::Vertex kTriangleVerts[] = {
			{.position = {  0.0f, -0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.5f, 0.0f }, .color = { 1.0f, 0.0f, 0.0f } },
			{.position = {  0.5f,  0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 0.0f, 1.0f, 0.0f } },
			{.position = { -0.5f,  0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 0.0f, 0.0f, 1.0f } },
		};
		constexpr std::uint32_t kTriangleIndices[] = { 0, 1, 2 };

		// -----------------------------------------------------------------------
		// Quad  (CCW, facing +Z)  – 4 unique verts, 2 triangles
		// -----------------------------------------------------------------------
		constexpr Mesh::Vertex kQuadVerts[] = {
			{.position = { -0.5f, -0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 1.0f, 0.0f, 0.0f } },
			{.position = {  0.5f, -0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 0.0f, 1.0f, 0.0f } },
			{.position = {  0.5f,  0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 0.0f, 0.0f, 1.0f } },
			{.position = { -0.5f,  0.5f, 0.0f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 1.0f, 1.0f, 0.0f } },
		};
		constexpr std::uint32_t kQuadIndices[] = { 0, 1, 2,  0, 2, 3 };

		// -----------------------------------------------------------------------
		// Cube  – 24 unique verts (4 per face), 36 indices (2 tris per face × 6)
		// Each face has its own normal and tangent.
		// -----------------------------------------------------------------------
		constexpr Mesh::Vertex kCubeVerts[] = {
			// +Z  normal=(0,0,1)  tangent=(1,0,0,1)
			{.position = { -0.5f, -0.5f,  0.5f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = {  0.5f, -0.5f,  0.5f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = { -0.5f,  0.5f,  0.5f }, .normal = { 0.0f, 0.0f, 1.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 1.0f, 0.2f, 0.2f } },
			// -Z  normal=(0,0,-1)  tangent=(-1,0,0,1)
			{.position = {  0.5f, -0.5f, -0.5f }, .normal = { 0.0f, 0.0f, -1.0f }, .tangent = { -1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = { -0.5f, -0.5f, -0.5f }, .normal = { 0.0f, 0.0f, -1.0f }, .tangent = { -1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .normal = { 0.0f, 0.0f, -1.0f }, .tangent = { -1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .normal = { 0.0f, 0.0f, -1.0f }, .tangent = { -1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 0.2f, 1.0f, 0.2f } },
			// +X  normal=(1,0,0)  tangent=(0,0,-1,1)
			{.position = {  0.5f, -0.5f,  0.5f }, .normal = { 1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, -1.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f, -0.5f }, .normal = { 1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, -1.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .normal = { 1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, -1.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .normal = { 1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, -1.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 0.2f, 0.2f, 1.0f } },
			// -X  normal=(-1,0,0)  tangent=(0,0,1,1)
			{.position = { -0.5f, -0.5f, -0.5f }, .normal = { -1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, 1.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f, -0.5f,  0.5f }, .normal = { -1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, 1.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f,  0.5f }, .normal = { -1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, 1.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .normal = { -1.0f, 0.0f, 0.0f }, .tangent = { 0.0f, 0.0f, 1.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 1.0f, 1.0f, 0.2f } },
			// +Y  normal=(0,1,0)  tangent=(1,0,0,1)
			{.position = { -0.5f,  0.5f,  0.5f }, .normal = { 0.0f, 1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .normal = { 0.0f, 1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .normal = { 0.0f, 1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .normal = { 0.0f, 1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 0.2f, 1.0f, 1.0f } },
			// -Y  normal=(0,-1,0)  tangent=(1,0,0,1)
			{.position = { -0.5f, -0.5f, -0.5f }, .normal = { 0.0f, -1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 1.0f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f, -0.5f }, .normal = { 0.0f, -1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 1.0f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f,  0.5f }, .normal = { 0.0f, -1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 1.0f, 0.0f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = { -0.5f, -0.5f,  0.5f }, .normal = { 0.0f, -1.0f, 0.0f }, .tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .uv = { 0.0f, 0.0f }, .color = { 1.0f, 0.2f, 1.0f } },
		};
		constexpr std::uint32_t kCubeIndices[] = {
			 0,  1,  2,   0,  2,  3,  // +Z
			 4,  5,  6,   4,  6,  7,  // -Z
			 8,  9, 10,   8, 10, 11,  // +X
			12, 13, 14,  12, 14, 15,  // -X
			16, 17, 18,  16, 18, 19,  // +Y
			20, 21, 22,  20, 22, 23,  // -Y
		};

		m_triangle = Mesh::Create(device, allocator, kTriangleVerts, kTriangleIndices);
		m_quad = Mesh::Create(device, allocator, kQuadVerts, kQuadIndices);
		m_cube = Mesh::Create(device, allocator, kCubeVerts, kCubeIndices);
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
		default:
			return m_triangle;
		}
	}
}
