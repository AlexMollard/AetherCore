#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <BinaryFormats.hpp>

#include "MeshProcessor.hpp"
#include "utils/BinaryReader.hpp"

using namespace aether;

namespace
{
	// A skinless animated model: "arm" is animated, "attach" is a plain offset node under it (no mesh, no
	// channel) and "parts" carries the mesh under that - tw-extract's exit-point attachment (the
	// shieldbearer's shield on his forearm).
	constexpr const char* kGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "arm", "children": [1]},
    {"name": "attach", "translation": [0.2, 0, 0], "children": [2]},
    {"name": "parts", "mesh": 0}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "animations": [{"name": "swing", "channels": [{"sampler": 0, "target": {"node": 0, "path": "rotation"}}],
    "samplers": [{"input": 2, "output": 3}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
    {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC4"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6},
    {"buffer": 0, "byteOffset": 44, "byteLength": 8},
    {"buffer": 0, "byteOffset": 52, "byteLength": 32}
  ],
  "buffers": [{"uri": "arm.bin", "byteLength": 84}]
})";

	// Bone name -> parent bone name ("" for a root), from a .skel blob.
	std::map<std::string, std::string> Parents(const std::vector<std::byte>& skel)
	{
		BinaryReader reader(skel);
		const auto hdr = reader.Read<SkelHeaderDisk>();
		reader.Skip(hdr.nameLen);
		std::vector<std::string> names;
		std::vector<std::int32_t> parents;
		for (std::uint32_t i = 0; i < hdr.boneCount; ++i)
		{
			const auto len = reader.Read<BoneEntryHeaderDisk>().nameLen;
			std::string name(len, '\0');
			reader.ReadRaw(name.data(), len);
			names.push_back(name);
			parents.push_back(reader.Read<std::int32_t>());
			reader.Skip(sizeof(float) * 16);
		}
		std::map<std::string, std::string> out;
		for (std::size_t i = 0; i < names.size(); ++i)
		{
			out[names[i]] = parents[i] >= 0 ? names[static_cast<std::size_t>(parents[i])] : "";
		}
		return out;
	}
}

TEST_CASE("A skinless model's mesh under an unanimated offset node still hangs off the animated bone above it")
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_attach_bake";
	std::filesystem::create_directories(dir);

	std::vector<std::byte> bin(84);
	const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const std::uint16_t idx[3] = {0, 1, 2};
	const float times[2] = {0, 1};
	const float rots[8] = {0, 0, 0, 1, 0, 0, 0.7071068f, 0.7071068f};
	std::memcpy(bin.data(), pos, sizeof(pos));
	std::memcpy(bin.data() + 36, idx, sizeof(idx));
	std::memcpy(bin.data() + 44, times, sizeof(times));
	std::memcpy(bin.data() + 52, rots, sizeof(rots));
	std::ofstream(dir / "arm.bin", std::ios::binary).write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));

	const std::string text = kGltf;
	const auto* p = reinterpret_cast<const std::byte*>(text.data());
	const auto result = assetpipeline::MeshProcessor::Process(std::span<const std::byte>(p, text.size()), dir / "arm.gltf", "models/arm.gltf", dir);
	REQUIRE_FALSE(result.skelData.empty());

	const auto parents = Parents(result.skelData);
	REQUIRE(parents.count("parts") == 1);
	CHECK(parents.at("parts") == "attach");
	REQUIRE(parents.count("attach") == 1);
	CHECK(parents.at("attach") == "arm");
	std::filesystem::remove_all(dir);
}
