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
	// Same shape as the vertex-colour bake test's glTF; the difference is the materials:
	// "leaf" carries alphaMode MASK plus tw-extract's {"foliage":true} extras, "lit" only
	// {"baked_lighting":true}, "plain" is opaque with no extras. "foam" is BLEND with the PS2
	// "blend add" preset tw-extract marks {"additive":true}, "glass" a plain BLEND that is also a
	// skydome layer ({"sky":true}), "solid" an opaque material wrongly carrying the additive mark,
	// and "beam" an additive prelit god-ray card ({"light_shaft":true}).
	constexpr const char* kGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1, 2, 3, 4, 5, 6]}],
  "nodes": [{"mesh": 0}, {"mesh": 1}, {"mesh": 2}, {"mesh": 3}, {"mesh": 4}, {"mesh": 5}, {"mesh": 6}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2, "material": 0}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 1}]},
    {"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2, "material": 2}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 3}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 4}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "material": 5}]},
    {"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2, "material": 6}]}
  ],
  "materials": [
    {"name": "leaf", "alphaMode": "MASK", "alphaCutoff": 0.5, "extras": {"foliage": true}},
    {"name": "plain"},
    {"name": "lit", "extras": {"baked_lighting": true}},
    {"name": "foam", "alphaMode": "BLEND", "extras": {"additive": true}},
    {"name": "glass", "alphaMode": "BLEND", "extras": {"sky": true}},
    {"name": "solid", "extras": {"additive": true}},
    {"name": "beam", "alphaMode": "BLEND", "extras": {"additive": true, "light_shaft": true, "baked_lighting": true}}
  ],
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

	MaterialHeaderDisk Header(const std::vector<std::pair<std::string, std::vector<std::byte>>>& files, const std::string& name)
	{
		for (const auto& [path, blob]: files)
		{
			if (path.find(name + ".material") != std::string::npos)
			{
				BinaryReader reader(blob);
				const auto hdr = reader.Read<MaterialHeaderDisk>();
				REQUIRE(CheckMagic(hdr));
				return hdr;
			}
		}
		FAIL("no generated material named " << name);
		return {};
	}
}

TEST_CASE("Baked glTF materials carry the foliage, baked-lighting and additive-blend marks exactly where tw-extract put them")
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_foliage_bake";
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

	CHECK(Header(result.materialFiles, "leaf").foliage != 0);
	CHECK(Header(result.materialFiles, "leaf").bakedLighting == 0);
	// Sky and baked lighting share one byte as separate bits; neither mark sets the other.
	CHECK(Header(result.materialFiles, "lit").bakedLighting == kMaterialDiskBaked);
	CHECK(Header(result.materialFiles, "glass").bakedLighting == kMaterialDiskSky);
	// The light-shaft bit rides the same byte and keeps the baked bit beside it.
	CHECK(Header(result.materialFiles, "beam").bakedLighting == (kMaterialDiskBaked | kMaterialDiskLightShaft));
	CHECK(Header(result.materialFiles, "beam").alphaBlend == 2);
	CHECK(Header(result.materialFiles, "lit").foliage == 0);
	CHECK(Header(result.materialFiles, "plain").foliage == 0);
	CHECK(Header(result.materialFiles, "plain").bakedLighting == 0);
	// alphaBlend: 0 opaque, 1 mix, 2 additive - and the additive mark alone never makes a draw blend.
	CHECK(Header(result.materialFiles, "foam").alphaBlend == 2);
	CHECK(Header(result.materialFiles, "glass").alphaBlend == 1);
	CHECK(Header(result.materialFiles, "solid").alphaBlend == 0);
	std::filesystem::remove_all(dir);
}
