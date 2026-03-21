#include "PrimitiveMeshes.hpp"

namespace meow
{
	void PrimitiveMeshes::Initialize(VkDevice device, VmaAllocator allocator)
	{
		constexpr Mesh::Vertex kTriangleVerts[] = {
			{.position = { 0.0f, -0.5f, 0.0f }, .color = { 1.0f, 0.0f, 0.0f } },
			{.position = { 0.5f,  0.5f, 0.0f }, .color = { 0.0f, 1.0f, 0.0f } },
			{.position = { -0.5f, 0.5f, 0.0f }, .color = { 0.0f, 0.0f, 1.0f } },
		};

		constexpr Mesh::Vertex kQuadVerts[] = {
			{.position = { -0.5f, -0.5f, 0.0f }, .color = { 1.0f, 0.0f, 0.0f } },
			{.position = {  0.5f, -0.5f, 0.0f }, .color = { 0.0f, 1.0f, 0.0f } },
			{.position = {  0.5f,  0.5f, 0.0f }, .color = { 0.0f, 0.0f, 1.0f } },
			{.position = { -0.5f, -0.5f, 0.0f }, .color = { 1.0f, 0.0f, 0.0f } },
			{.position = {  0.5f,  0.5f, 0.0f }, .color = { 0.0f, 0.0f, 1.0f } },
			{.position = { -0.5f,  0.5f, 0.0f }, .color = { 1.0f, 1.0f, 0.0f } },
		};

		constexpr Mesh::Vertex kCubeVerts[] = {
			// +Z
			{.position = { -0.5f, -0.5f,  0.5f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = {  0.5f, -0.5f,  0.5f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = { -0.5f, -0.5f,  0.5f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .color = { 1.0f, 0.2f, 0.2f } },
			{.position = { -0.5f,  0.5f,  0.5f }, .color = { 1.0f, 0.2f, 0.2f } },
			// -Z
			{.position = {  0.5f, -0.5f, -0.5f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = { -0.5f, -0.5f, -0.5f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = {  0.5f, -0.5f, -0.5f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .color = { 0.2f, 1.0f, 0.2f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .color = { 0.2f, 1.0f, 0.2f } },
			// +X
			{.position = {  0.5f, -0.5f,  0.5f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f, -0.5f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f,  0.5f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .color = { 0.2f, 0.2f, 1.0f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .color = { 0.2f, 0.2f, 1.0f } },
			// -X
			{.position = { -0.5f, -0.5f, -0.5f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f, -0.5f,  0.5f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f,  0.5f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f, -0.5f, -0.5f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f,  0.5f }, .color = { 1.0f, 1.0f, 0.2f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .color = { 1.0f, 1.0f, 0.2f } },
			// +Y
			{.position = { -0.5f,  0.5f,  0.5f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = {  0.5f,  0.5f,  0.5f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = { -0.5f,  0.5f,  0.5f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = {  0.5f,  0.5f, -0.5f }, .color = { 0.2f, 1.0f, 1.0f } },
			{.position = { -0.5f,  0.5f, -0.5f }, .color = { 0.2f, 1.0f, 1.0f } },
			// -Y
			{.position = { -0.5f, -0.5f, -0.5f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f, -0.5f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f,  0.5f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = { -0.5f, -0.5f, -0.5f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = {  0.5f, -0.5f,  0.5f }, .color = { 1.0f, 0.2f, 1.0f } },
			{.position = { -0.5f, -0.5f,  0.5f }, .color = { 1.0f, 0.2f, 1.0f } },
		};

		m_triangle = Mesh::Create(device, allocator, kTriangleVerts);
		m_quad = Mesh::Create(device, allocator, kQuadVerts);
		m_cube = Mesh::Create(device, allocator, kCubeVerts);
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
