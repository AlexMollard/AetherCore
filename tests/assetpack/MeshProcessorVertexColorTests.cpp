#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <BinaryFormats.hpp>

#include "MeshProcessor.hpp"
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

TEST_CASE("Baked glTF materials modulate by vertex colour exactly where a primitive carries COLOR_0")
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_vtxcolor_bake";
	std::filesystem::create_directories(dir);

	std::vector<std::byte> bin(92);
	const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float col[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
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
	std::filesystem::remove_all(dir);
}
