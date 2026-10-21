#include "mesh/PrimitiveMeshData.hpp"

#include <array>
#include <cmath>
#include <doctest/doctest.h>

using namespace aether;

TEST_CASE("PrimitiveMeshData preserves native topology and valid indices")
{
	struct PrimitiveCase
	{
		PrimitiveMesh primitive;
		std::size_t vertices;
		std::size_t indices;
	};

	constexpr PrimitiveCase kCases[] = {
	        {PrimitiveMesh::Triangle, 3, 3},
	        {PrimitiveMesh::Quad, 4, 6},
	        {PrimitiveMesh::Cube, 24, 36},
	        {PrimitiveMesh::Plane, 441, 2400},
	        {PrimitiveMesh::Sphere, 561, 2880},
	};
	for (const auto& item: kCases)
	{
		CAPTURE(static_cast<int>(item.primitive));
		const auto mesh = GeneratePrimitiveMesh(item.primitive);
		REQUIRE(mesh.has_value());
		CHECK(mesh->vertices.size() == item.vertices);
		CHECK(mesh->indices.size() == item.indices);
		REQUIRE(mesh->indices.size() % 3 == 0);
		for (const auto index: mesh->indices)
		{
			REQUIRE(index < mesh->vertices.size());
		}
		for (std::size_t index = 0; index < mesh->indices.size(); index += 3)
		{
			const auto& a = mesh->vertices[mesh->indices[index]];
			const auto& b = mesh->vertices[mesh->indices[index + 1]];
			const auto& c = mesh->vertices[mesh->indices[index + 2]];
			CHECK(glm::dot(glm::cross(b.position - a.position, c.position - a.position), a.normal + b.normal + c.normal) > 0.0f);
		}
	}
	CHECK_FALSE(GeneratePrimitiveMesh(static_cast<PrimitiveMesh>(-1)).has_value());
	CHECK_FALSE(GeneratePrimitiveMesh(static_cast<PrimitiveMesh>(5)).has_value());
}

TEST_CASE("PrimitiveMeshData triangle retains orientation UVs and vertex colors")
{
	const auto mesh = GeneratePrimitiveMesh(PrimitiveMesh::Triangle);
	REQUIRE(mesh.has_value());
	const std::array positions = {glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 0.0f), glm::vec3(-0.5f, 0.5f, 0.0f)};
	const std::array uvs = {glm::vec2(0.5f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f)};
	const std::array colors = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
	REQUIRE(mesh->vertices.size() == positions.size());
	CHECK(mesh->indices == std::vector<std::uint32_t>{0, 1, 2});
	for (std::size_t index = 0; index < positions.size(); ++index)
	{
		CHECK(mesh->vertices[index].position == positions[index]);
		CHECK(mesh->vertices[index].uv == uvs[index]);
		CHECK(mesh->vertices[index].color == colors[index]);
		CHECK(mesh->vertices[index].normal == glm::vec3(0.0f, 0.0f, 1.0f));
		CHECK(mesh->vertices[index].tangent == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	}
}

TEST_CASE("PrimitiveMeshData quad and cube retain face UVs colors and index order")
{
	const std::array uvs = {glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 0.0f)};
	const std::array normals = {glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0)};
	const std::array colors = {glm::vec3(1.0f, 0.2f, 0.2f), glm::vec3(0.2f, 1.0f, 0.2f), glm::vec3(0.2f, 0.2f, 1.0f), glm::vec3(1.0f, 1.0f, 0.2f), glm::vec3(0.2f, 1.0f, 1.0f), glm::vec3(1.0f, 0.2f, 1.0f)};
	const std::array tangents = {glm::vec4(1, 0, 0, 1), glm::vec4(-1, 0, 0, 1), glm::vec4(0, 0, -1, 1), glm::vec4(0, 0, 1, 1), glm::vec4(1, 0, 0, 1), glm::vec4(1, 0, 0, 1)};
	const glm::vec3 cubePositions[] = {
	        {-0.5f, -0.5f, 0.5f},
	        {0.5f, -0.5f, 0.5f},
	        {0.5f, 0.5f, 0.5f},
	        {-0.5f, 0.5f, 0.5f},
	        {0.5f, -0.5f, -0.5f},
	        {-0.5f, -0.5f, -0.5f},
	        {-0.5f, 0.5f, -0.5f},
	        {0.5f, 0.5f, -0.5f},
	        {0.5f, -0.5f, 0.5f},
	        {0.5f, -0.5f, -0.5f},
	        {0.5f, 0.5f, -0.5f},
	        {0.5f, 0.5f, 0.5f},
	        {-0.5f, -0.5f, -0.5f},
	        {-0.5f, -0.5f, 0.5f},
	        {-0.5f, 0.5f, 0.5f},
	        {-0.5f, 0.5f, -0.5f},
	        {-0.5f, 0.5f, 0.5f},
	        {0.5f, 0.5f, 0.5f},
	        {0.5f, 0.5f, -0.5f},
	        {-0.5f, 0.5f, -0.5f},
	        {-0.5f, -0.5f, -0.5f},
	        {0.5f, -0.5f, -0.5f},
	        {0.5f, -0.5f, 0.5f},
	        {-0.5f, -0.5f, 0.5f},
	};
	constexpr std::uint32_t kFaceIndices[] = {0, 1, 2, 0, 2, 3};
	for (const auto primitive: {PrimitiveMesh::Quad, PrimitiveMesh::Cube})
	{
		const auto mesh = GeneratePrimitiveMesh(primitive);
		REQUIRE(mesh.has_value());
		const std::size_t faces = primitive == PrimitiveMesh::Cube ? 6 : 1;
		REQUIRE(mesh->vertices.size() == faces * 4);
		REQUIRE(mesh->indices.size() == faces * 6);
		for (std::size_t face = 0; face < faces; ++face)
		{
			for (std::size_t corner = 0; corner < 4; ++corner)
			{
				const auto& vertex = mesh->vertices[face * 4 + corner];
				CHECK(vertex.uv == uvs[corner]);
				CHECK(vertex.normal == normals[face]);
				CHECK(vertex.tangent == tangents[face]);
				CHECK(vertex.color == (primitive == PrimitiveMesh::Cube ? colors[face] : glm::vec3(1.0f)));
				CHECK(std::abs(vertex.position.x) == 0.5f);
				CHECK(std::abs(vertex.position.y) == 0.5f);
				if (primitive == PrimitiveMesh::Cube)
				{
					CHECK(vertex.position == cubePositions[face * 4 + corner]);
					CHECK(std::abs(vertex.position.z) == 0.5f);
					CHECK(glm::dot(vertex.position, vertex.normal) == 0.5f);
				}
				else
				{
					CHECK(vertex.position == glm::vec3(cubePositions[corner].x, cubePositions[corner].y, 0.0f));
				}
			}
			for (std::size_t index = 0; index < 6; ++index)
			{
				CHECK(mesh->indices[face * 6 + index] == face * 4 + kFaceIndices[index]);
			}
		}
	}
}

TEST_CASE("PrimitiveMeshData plane retains tiled UVs and sphere retains its seam")
{
	const auto plane = GeneratePrimitiveMesh(PrimitiveMesh::Plane);
	REQUIRE(plane.has_value());
	REQUIRE(plane->vertices.size() == 441);
	CHECK(plane->vertices.front().position == glm::vec3(-0.5f, -0.5f, 0.0f));
	CHECK(plane->vertices.back().position == glm::vec3(0.5f, 0.5f, 0.0f));
	CHECK(plane->vertices.front().uv == glm::vec2(0.0f, 20.0f));
	CHECK(plane->vertices.back().uv == glm::vec2(20.0f, 0.0f));
	for (const auto& vertex: plane->vertices)
	{
		CHECK(vertex.color == glm::vec3(1.0f));
		CHECK(vertex.normal == glm::vec3(0.0f, 0.0f, 1.0f));
	}
	const auto sphere = GeneratePrimitiveMesh(PrimitiveMesh::Sphere);
	REQUIRE(sphere.has_value());
	REQUIRE(sphere->vertices.size() == 561);
	for (std::size_t stack = 0; stack <= 16; ++stack)
	{
		const auto& start = sphere->vertices[stack * 33];
		const auto& end = sphere->vertices[stack * 33 + 32];
		CHECK(glm::length(start.position - end.position) < 0.000001f);
		CHECK(start.uv.x == 0.0f);
		CHECK(end.uv.x == 1.0f);
		CHECK(start.uv.y == float(stack) / 16.0f);
	}
	for (const auto& vertex: sphere->vertices)
	{
		CHECK(glm::length(vertex.position) == doctest::Approx(0.5f));
		CHECK(glm::length(vertex.normal) == doctest::Approx(1.0f));
		CHECK(vertex.color == glm::vec3(1.0f));
	}
}
