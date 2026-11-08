#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <BinaryFormats.hpp>

#include "MeshProcessor.hpp"
#include "assets/GltfAsset.hpp"
#include "io/FileSystem.hpp"
#include "utils/BinaryReader.hpp"

using namespace aether;

namespace
{
	// One triangle drawn twice: once with COLOR_0 (material "lit"), once without (material "plain").
	constexpr const char* kGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1]}],
  "nodes": [{"mesh": 0}, {"mesh": 1}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2, "material": 0}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 1}]}
  ],
  "materials": [{"name": "lit"}, {"name": "plain"}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 48},
    {"buffer": 0, "byteOffset": 84, "byteLength": 6}
  ],
  "buffers": [{"uri": "tri.bin", "byteLength": 92}]
})";

	bool VertexColorFlag(const std::vector<std::pair<std::string, std::vector<std::byte>>>& files, const std::string& name)
	{
		for (const auto& [path, blob]: files)
		{
			if (path.find(name + ".material") != std::string::npos)
			{
				BinaryReader reader(blob);
				const auto hdr = reader.Read<MaterialHeaderDisk>();
				REQUIRE(CheckMagic(hdr));
				return hdr.modulateVertexColor != 0;
			}
		}
		FAIL("no generated material named " << name);
		return false;
	}
}

TEST_CASE("Prepared browser mesh versions preserve legacy layout and RGBA vertex opacity")
{
	const std::string metadata = R"({"materials":[],"skins":[],"animations":[],"bounds":[0,0,0,1,1,0,0.5,0.5,0,1],
"nodes":[{"name":"triangle","parentIndex":-1,"meshIndex":0,"skinIndex":-1,"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1],"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"hasMatrix":false,"children":[]}],
"primitives":[{"materialIndex":-1,"skinIndex":-1,"primitiveKey":"alpha","vertexCount":3,"indexCount":3}]})";
	const std::uint32_t indices[] = {2, 0, 1};
	for (const bool legacy: {true, false})
	{
		CAPTURE(legacy);
		const std::size_t stride = legacy ? 100 : 104;
		const auto metadataBytes = static_cast<std::uint32_t>(metadata.size());
		const auto streamBytes = static_cast<std::uint32_t>(3 * stride + sizeof(indices));
		std::vector<std::byte> data(16 + metadataBytes + streamBytes);
		std::memcpy(data.data(), legacy ? "AEP2" : "AEP3", 4);
		std::memcpy(data.data() + 4, &metadataBytes, 4);
		std::memcpy(data.data() + 8, &streamBytes, 4);
		std::memcpy(data.data() + 16, metadata.data(), metadata.size());
		for (std::size_t i = 0; i < 3; ++i)
		{
			MeshVertex vertex{};
			vertex.position = glm::vec3(static_cast<float>(i), 2.0f, 3.0f);
			vertex.color = glm::vec3(0.25f, 0.5f, 0.75f);
			vertex.colorAlpha = static_cast<float>(i) * 0.5f;
			std::memcpy(data.data() + 16 + metadataBytes + i * stride, &vertex, stride);
		}
		std::memcpy(data.data() + 16 + metadataBytes + 3 * stride, indices, sizeof(indices));
		const auto loaded = assets::GltfAsset::FromPrepared(data, "vertex_alpha_fixture://prepared.mesh");
		REQUIRE(loaded.has_value());
		REQUIRE(loaded->primitives.size() == 1);
		const auto& primitive = loaded->primitives.front();
		REQUIRE(primitive.vertices.size() == 3);
		CHECK(primitive.indices == std::vector<std::uint32_t>{2, 0, 1});
		for (std::size_t i = 0; i < primitive.vertices.size(); ++i)
		{
			const auto& vertex = primitive.vertices[i];
			CHECK(vertex.position == glm::vec3(static_cast<float>(i), 2.0f, 3.0f));
			CHECK(vertex.color == glm::vec3(0.25f, 0.5f, 0.75f));
			CHECK(vertex.jointWeights == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(vertex.colorAlpha == (legacy ? 1.0f : static_cast<float>(i) * 0.5f));
		}
		if (!legacy)
		{
			const float nonfinite = std::numeric_limits<float>::quiet_NaN();
			std::memcpy(data.data() + 16 + metadataBytes + 100, &nonfinite, sizeof(nonfinite));
			CHECK_FALSE(assets::GltfAsset::FromPrepared(data, "vertex_alpha_fixture://nonfinite.mesh").has_value());
		}
		data.pop_back();
		CHECK_FALSE(assets::GltfAsset::FromPrepared(data, "vertex_alpha_fixture://truncated.mesh").has_value());
	}
}

TEST_CASE("Baked glTF materials modulate by vertex colour exactly where a primitive carries COLOR_0")
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_vtxcolor_bake";
	std::filesystem::create_directories(dir);

	std::vector<std::byte> bin(92);
	const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float col[12] = {1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1, 1};
	const std::uint16_t idx[3] = {0, 1, 2};
	std::memcpy(bin.data(), pos, sizeof(pos));
	std::memcpy(bin.data() + 36, col, sizeof(col));
	std::memcpy(bin.data() + 84, idx, sizeof(idx));
	std::ofstream(dir / "tri.bin", std::ios::binary).write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));

	const std::string text = kGltf;
	const auto* p = reinterpret_cast<const std::byte*>(text.data());
	const auto result = assetpipeline::MeshProcessor::Process(std::span<const std::byte>(p, text.size()), dir / "tri.gltf", "models/tri.gltf", dir);
	REQUIRE_FALSE(result.meshData.empty());

	CHECK(VertexColorFlag(result.materialFiles, "lit"));
	CHECK_FALSE(VertexColorFlag(result.materialFiles, "plain"));
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("vertex_alpha_fixture", dir);
	const auto loaded = assets::GltfAsset::LoadFromMemory(result.meshData, "vertex_alpha_fixture://vertex-alpha-test.mesh");
	REQUIRE(loaded.has_value());
	REQUIRE(loaded->primitives.size() == 2);
	bool transparent = false, partial = false, opaque = false;
	for (const auto& vertex: loaded->primitives.front().vertices)
	{
		transparent |= vertex.colorAlpha == 0.0f;
		// The existing RGBA8 baker truncates 0.5 * 255 to 127.
		partial |= vertex.colorAlpha == doctest::Approx(127.0f / 255.0f);
		opaque |= vertex.colorAlpha == 1.0f;
	}
	CHECK(transparent);
	CHECK(partial);
	CHECK(opaque);
	for (const auto& vertex: loaded->primitives.back().vertices)
	{
		CHECK(vertex.colorAlpha == 1.0f);
	}
	std::filesystem::remove_all(dir);
}
