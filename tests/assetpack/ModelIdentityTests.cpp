#include <doctest/doctest.h>
#include <BinaryFormats.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>
#include "MeshProcessor.hpp"
#include "AssetPipeline.hpp"
#include "PakWriter.hpp"
#include "assets/GltfAsset.hpp"
#include "io/FileUtil.hpp"
#include "io/MemoryPak.hpp"

TEST_CASE("Cooked model logical identity survives geometry edits and named primitive reorder")
{
	using namespace aether;
	const auto root = std::filesystem::temp_directory_path() / ("ae-model-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

	struct Cleanup
	{
		std::filesystem::path root;

		~Cleanup()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup{root};

	std::filesystem::create_directories(root);
	auto document = nlohmann::json::parse(R"({
		"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],
		"nodes":[{"name":"Body","mesh":0},{"name":"Wheel","mesh":0}],
		"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
		"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
		"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
		"buffers":[{"uri":"geometry.bin","byteLength":44}]
	})");
	const auto bake = [&](float width)
	{
		std::array<std::byte, 44> binary{};
		const float positions[9] = {0, 0, 0, width, 0, 0, 0, 1, 0};
		const std::uint16_t indices[3] = {0, 1, 2};
		std::memcpy(binary.data(), positions, sizeof(positions));
		std::memcpy(binary.data() + 36, indices, sizeof(indices));
		std::ofstream(root / "geometry.bin", std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(binary.data()), binary.size());
		const auto text = document.dump();
		std::ofstream(root / "model.gltf", std::ios::binary | std::ios::trunc) << text;
		const auto packed = assetpipeline::MeshProcessor::Process({reinterpret_cast<const std::byte*>(text.data()), text.size()}, root / "model.gltf", "model.gltf", root);
		REQUIRE_FALSE(packed.meshData.empty());
		if (!std::filesystem::exists(root / "model.mesh"))
		{
			// Retain the first bake so the package must prefer the current source cook.
			std::ofstream(root / "model.mesh", std::ios::binary).write(reinterpret_cast<const char*>(packed.meshData.data()), packed.meshData.size());
		}
		const auto model = assets::GltfAsset::LoadFromMemory(packed.meshData, "model.mesh");
		REQUIRE(model.has_value());
		return *model;
	};
	const auto original = bake(1.0f);
	REQUIRE(original.primitives.size() == 2);
	const auto body = assets::GltfAsset::PrimitiveKey(original.primitives[0]);
	const auto wheel = assets::GltfAsset::PrimitiveKey(original.primitives[1]);
	REQUIRE(body != wheel);
	REQUIRE(body != "unidentified");
	REQUIRE(wheel != "unidentified");
	document["nodes"][0]["name"] = "Wheel";
	document["nodes"][1]["name"] = "Body";
	const auto reimported = bake(2.0f);
	REQUIRE(reimported.primitives.size() == 2);
	CHECK(assets::GltfAsset::PrimitiveKey(reimported.primitives[0]) == wheel);
	CHECK(assets::GltfAsset::PrimitiveKey(reimported.primitives[1]) == body);
	CHECK(assets::GltfAsset::GeometryPrimitiveKey(reimported.primitives[0]) != assets::GltfAsset::GeometryPrimitiveKey(original.primitives[1]));
	document["nodes"].erase(0);
	document["scenes"][0]["nodes"] = nlohmann::json::array({0});
	const auto removed = bake(2.0f);
	REQUIRE(removed.primitives.size() == 1);
	CHECK(assets::GltfAsset::PrimitiveKey(removed.primitives[0]) == body);
	CHECK(assets::GltfAsset::PrimitiveKey(removed.primitives[0]) != wheel);

	const auto packagePath = root / "export.pak";
	REQUIRE(assetpipeline::PackDirectory(root, packagePath, {.target = "webgpu-2d-v1"}).ok);
	const auto bytes = io::file_util::ReadBinary(packagePath);
	REQUIRE(bytes.has_value());
	const auto package = io::MemoryPak::Open(*bytes, "webgpu-2d-v1");
	REQUIRE(package.has_value());
	const auto packedMesh = package->Read("model.mesh");
	REQUIRE(packedMesh.has_value());
	const auto shipped = assets::GltfAsset::LoadFromMemory(*packedMesh, "model.mesh");
	REQUIRE(shipped.has_value());
	REQUIRE(shipped->primitives.size() == 1);
	CHECK(assets::GltfAsset::PrimitiveKey(shipped->primitives.front()) == body);
	CHECK(assets::GltfAsset::GeometryPrimitiveKey(shipped->primitives.front()) == assets::GltfAsset::GeometryPrimitiveKey(removed.primitives.front()));

	std::filesystem::create_directories(root / "first");
	std::filesystem::create_directories(root / "second");
	std::ofstream(root / "first/shared.spv", std::ios::binary) << "first shader";
	std::ofstream(root / "second/shared.spv", std::ios::binary) << "conflicting shader";
	assetpipeline::PakWriter conflicting;
	conflicting.AddDirectoryAs(root / "first", "shaders");
	conflicting.AddDirectoryAs(root / "second", "shaders");
	CHECK_FALSE(conflicting.Write(packagePath));
	const auto preserved = io::file_util::ReadBinary(packagePath);
	REQUIRE(preserved.has_value());
	CHECK(*preserved == *bytes);

	const auto materialDirectory = root / "override";
	std::filesystem::create_directory(materialDirectory);
	const MaterialHeaderDisk cachedMaterial{};
	std::ofstream(materialDirectory / "Body.material", std::ios::binary).write(reinterpret_cast<const char*>(&cachedMaterial), sizeof(cachedMaterial));
	std::ofstream(materialDirectory / "Body.material.override.toml", std::ios::binary) << "[material]\nuvScroll = [0.055, 0.0]\n";
	const auto materialPackagePath = root / "materials.pak";
	REQUIRE(assetpipeline::PackDirectory(materialDirectory, materialPackagePath, {}).ok);
	const auto materialBytes = io::file_util::ReadBinary(materialPackagePath);
	REQUIRE(materialBytes.has_value());
	const auto materialPackage = io::MemoryPak::Open(*materialBytes, "desktop");
	REQUIRE(materialPackage.has_value());
	const auto material = materialPackage->Read("Body.material");
	REQUIRE(material.has_value());
	REQUIRE(material->size() >= sizeof(MaterialHeaderDisk));
	MaterialHeaderDisk shippedMaterial{};
	std::memcpy(&shippedMaterial, material->data(), sizeof(shippedMaterial));
	CHECK(shippedMaterial.uvScroll[0] == doctest::Approx(0.055f));
}
