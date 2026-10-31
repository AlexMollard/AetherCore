#include "mesh/GrassMeshData.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <doctest/doctest.h>

using namespace aether;

namespace
{
	bool ContainsChunk(const std::vector<MeshGen::GrassChunkKey>& chunks, MeshGen::GrassChunkKey wanted)
	{
		// Predicate search avoids an MSVC STL/Clang vectorized-find dispatch that
		// selects an unsupported 12-byte element size for this aggregate key.
		return std::ranges::any_of(chunks, [wanted](const auto& key) { return key == wanted; });
	}

	MeshGen::MeshData Rectangle(float x0, float z0, float x1, float z1)
	{
		MeshGen::MeshData mesh;
		for (const auto p: {glm::vec2{x0, z0}, glm::vec2{x1, z0}, glm::vec2{x0, z1}, glm::vec2{x1, z1}})
		{
			mesh.vertices.push_back({.position = {p.x, p.x * 0.25f + p.y * 0.5f, p.y}});
		}
		mesh.indices = {0, 2, 1, 1, 2, 3};
		return mesh;
	}

	std::vector<glm::vec3> Anchors(const MeshGen::GrassChunk& chunk)
	{
		std::vector<glm::vec3> result;
		for (std::size_t i = 0; i < chunk.mesh.vertices.size(); i += 5)
		{
			result.push_back(chunk.origin + (chunk.mesh.vertices[i].position + chunk.mesh.vertices[i + 1].position) * 0.5f);
		}
		return result;
	}
}

TEST_CASE("Grass projects onto terrain and excludes paths and holes")
{
	GrassComponent s;
	s.regionMin = {0, 0}; s.regionMax = {6, 4}; s.spacing = 1;
	const auto terrain = Rectangle(0, 0, 4, 4), path = Rectangle(1, 0, 3, 4);
	auto field = MeshGen::GrassField::Create(s, {terrain.vertices, terrain.indices}, {path.vertices, path.indices});
	REQUIRE(field.has_value());
	std::size_t count = 0;
	for (unsigned lod = 0; lod < 3; ++lod)
	{
		auto chunk = field->GenerateChunk({0, 0, lod});
		REQUIRE(chunk.has_value());
		for (const auto p: Anchors(*chunk))
		{
			++count;
			CHECK(p.x >= 0); CHECK(p.x < 4); CHECK((p.x < 1 || p.x > 3));
			CHECK(p.y == doctest::Approx(p.x * 0.25f + p.z * 0.5f));
		}
	}
	CHECK(count == 8);
	auto hole = field->GenerateChunk({1, 0, 0});
	REQUIRE(hole.has_value()); CHECK(hole->mesh.vertices.empty());
}

TEST_CASE("Grass layers are disjoint and stable across region and chunk seams")
{
	GrassComponent s; s.regionMin = {-8, -8}; s.regionMax = {8, 8}; s.spacing = 1; s.seed = 193;
	auto field = MeshGen::GrassField::Create(s); REQUIRE(field.has_value());
	std::vector<glm::vec3> roots;
	for (int z = -2; z < 2; ++z)
	{
		for (int x = -2; x < 2; ++x)
		{
			for (unsigned lod = 0; lod < 3; ++lod)
			{
				auto chunk = field->GenerateChunk({x, z, lod}); REQUIRE(chunk.has_value());
				for (const auto p: Anchors(*chunk))
				{
					CHECK(std::ranges::none_of(roots, [p](auto old) { return glm::distance(p, old) < 1e-5f; }));
					roots.push_back(p);
				}
			}
		}
	}
	CHECK(roots.size() == 256);
	s.regionMin = {0, 0};
	auto smaller = MeshGen::GrassField::Create(s); REQUIRE(smaller.has_value());
	for (unsigned lod = 0; lod < 3; ++lod)
	{
		auto a = field->GenerateChunk({0, 0, lod}), b = smaller->GenerateChunk({0, 0, lod});
		REQUIRE(a.has_value()); REQUIRE(b.has_value());
		const auto aa = Anchors(*a), bb = Anchors(*b); REQUIRE(aa.size() == bb.size());
		for (std::size_t i = 0; i < aa.size(); ++i)
		{
			CHECK(glm::distance(aa[i], bb[i]) < 1e-5f);
		}
	}
}

TEST_CASE("Grass blades are curved anchored geometry")
{
	GrassComponent s; s.spacing = 1;
	auto field = MeshGen::GrassField::Create(s); REQUIRE(field.has_value());
	auto chunk = field->GenerateChunk({0, 0, 0}); REQUIRE(chunk.has_value());
	REQUIRE_FALSE(chunk->mesh.vertices.empty());
	CHECK(chunk->mesh.indices.size() * 5 == chunk->mesh.vertices.size() * 9);
	for (std::size_t i = 0; i < chunk->mesh.vertices.size(); i += 5)
	{
		const auto& v = chunk->mesh.vertices;
		CHECK(v[i].uv2.x == 0); CHECK(v[i + 1].uv2.x == 0);
		CHECK(v[i + 2].uv2.x == doctest::Approx(0.55)); CHECK(v[i + 4].uv2.x == 1);
		CHECK(v[i].uv2.y == v[i + 4].uv2.y);
		const auto root = (v[i].position + v[i + 1].position) * 0.5f;
		const auto mid = (v[i + 2].position + v[i + 3].position) * 0.5f;
		CHECK(glm::length(glm::cross(mid - root, v[i + 4].position - root)) > 1e-6f);
	}
}

TEST_CASE("Grass camera planner stays bounded and deterministic in huge regions")
{
	GrassComponent s; s.regionMin = {-100000, -100000}; s.regionMax = {100000, 100000};
	auto field = MeshGen::GrassField::Create(s); REQUIRE(field.has_value());
	std::vector<MeshGen::GrassChunkKey> a, b;
	REQUIRE(field->SelectChunks({0.1f, 0.2f}, a).has_value());
	REQUIRE(field->SelectChunks({0.9f, 0.8f}, b).has_value());
	CHECK_FALSE(a.empty()); CHECK(a.size() < 2000); CHECK(a == b);
	CHECK(field->DistanceFade(0).x == doctest::Approx(7.5));
	CHECK(field->DistanceFade(2).y == doctest::Approx(44));
	REQUIRE(field->SelectChunks({0.1f, 0.2f}, b, 2.0f).has_value());
	CHECK(b.size() > a.size());
	for (const auto& key: a)
	{
		CHECK(ContainsChunk(b, key));
	}
	CHECK(ContainsChunk(b, MeshGen::GrassChunkKey{20, 0, 2}));
	CHECK_FALSE(ContainsChunk(a, MeshGen::GrassChunkKey{20, 0, 2}));
	REQUIRE(field->SelectChunks({3.99f, 0}, b, 0.01f).has_value());
	CHECK(ContainsChunk(b, MeshGen::GrassChunkKey{1, 0, 2}));
	CHECK_FALSE(field->SelectChunks({0, 0}, b, 0).has_value());
	CHECK(b.empty());
	CHECK_FALSE(field->SelectChunks({0, 0}, b, std::numeric_limits<float>::infinity()).has_value());
	CHECK(b.empty());
	CHECK_FALSE(field->SelectChunks({0, 0}, b, 1000).has_value());
	CHECK(b.empty());
}

TEST_CASE("Grass validates inputs and source geometry")
{
	GrassComponent s;
	SUBCASE("Zero spacing") { s.spacing = 0; }
	SUBCASE("Nonfinite bounds") { s.regionMax.x = std::numeric_limits<float>::infinity(); }
	SUBCASE("Reversed height") { s.heightRange = {1, 0.5f}; }
	SUBCASE("Unbounded chunk") { s.chunkSize = 1000; }
	SUBCASE("Unbounded window") { s.drawDistance = 100000; }
	CHECK_FALSE(MeshGen::GrassField::Create(s).has_value());
}

TEST_CASE("Grass rejects malformed terrain indices")
{
	auto terrain = Rectangle(-10, -10, 10, 10); terrain.indices[0] = 100;
	CHECK_FALSE(MeshGen::GrassField::Create(GrassComponent{}, {terrain.vertices, terrain.indices}).has_value());
}

TEST_CASE("Grass interpolates surface masks and keys runtime settings")
{
	GrassComponent s;
	s.regionMin = {0, 0}; s.regionMax = {4, 4}; s.spacing = 1; s.surfaceMask = true;
	auto terrain = Rectangle(0, 0, 4, 4);
	for (auto& vertex: terrain.vertices)
	{
		vertex.color = {0, 1, 0};
	}
	auto empty = MeshGen::GrassField::Create(s, {terrain.vertices, terrain.indices});
	REQUIRE(empty.has_value());
	auto noGrass = empty->GenerateChunk({0, 0, 0}); REQUIRE(noGrass.has_value());
	CHECK(noGrass->mesh.vertices.empty());
	s.heightRange = {1, 1};
	for (auto& vertex: terrain.vertices)
	{
		vertex.color = {1, 0.25f + vertex.position.x * 0.1875f, 1};
	}
	auto masked = MeshGen::GrassField::Create(s, {terrain.vertices, terrain.indices});
	REQUIRE(masked.has_value());
	auto grass = masked->GenerateChunk({0, 0, 0}); REQUIRE(grass.has_value());
	REQUIRE_FALSE(grass->mesh.vertices.empty());
	for (std::size_t i = 0; i < grass->mesh.vertices.size(); i += 5)
	{
		const auto& vertices = grass->mesh.vertices;
		const float x = grass->origin.x + (vertices[i].position.x + vertices[i + 1].position.x) * 0.5f;
		CHECK(vertices[i].uv2.y == doctest::Approx(0.25f + x * 0.1875f));
	}
	const auto key = MeshGen::GrassMeshKey(s);
	s.enabled = false; CHECK(MeshGen::GrassMeshKey(s) == key);
	s.windStrength += 1; CHECK(MeshGen::GrassMeshKey(s) == key);
}
