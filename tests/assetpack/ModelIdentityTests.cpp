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
#include "BakeAll.hpp"
#include "BakeOutputs.hpp"
#include "AssetPipeline.hpp"
#include "PakWriter.hpp"
#include "assets/GltfAsset.hpp"
#include "io/FileUtil.hpp"
#include "io/FileSystem.hpp"
#include "io/MemoryPak.hpp"

namespace
{
	aether::assetpipeline::ByteBuffer ReadBakeBytes(const std::filesystem::path& path)
	{
		const auto bytes = aether::io::file_util::ReadBinary(path);
		REQUIRE(bytes.has_value());
		return *bytes;
	}

	struct ModelBakeFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / ("ae-bake-integrity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		nlohmann::json document = nlohmann::json::parse(R"({
			"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Body","mesh":0}],
			"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
			"materials":[{"name":"Shared","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],
			"animations":[{"name":"turn","channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}}],"samplers":[{"input":2,"output":3}]}],
			"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
			{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":2,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
			{"bufferView":3,"componentType":5126,"count":2,"type":"VEC4"}],
			"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6},
			{"buffer":0,"byteOffset":44,"byteLength":8},{"buffer":0,"byteOffset":52,"byteLength":32}],"buffers":[{"uri":"geometry.bin","byteLength":84}]
		})");

		ModelBakeFixture()
		{
			std::filesystem::create_directories(root / "assets/models");
			std::array<std::byte, 84> binary{};
			const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
			const std::uint16_t indices[3] = {0, 1, 2};
			const float times[2] = {0, 1};
			const float rotations[8] = {0, 0, 0, 1, 0, 0, 0.7071068f, 0.7071068f};
			std::memcpy(binary.data(), positions, sizeof(positions));
			std::memcpy(binary.data() + 36, indices, sizeof(indices));
			std::memcpy(binary.data() + 44, times, sizeof(times));
			std::memcpy(binary.data() + 52, rotations, sizeof(rotations));
			std::ofstream(root / "assets/models/geometry.bin", std::ios::binary).write(reinterpret_cast<const char*>(binary.data()), binary.size());
			WriteModel("first");
		}

		~ModelBakeFixture()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}

		void WriteModel(const std::string& name)
		{
			std::ofstream(root / "assets/models" / (name + ".gltf"), std::ios::binary | std::ios::trunc) << document.dump();
		}

		aether::assetpipeline::MeshProcessor::ProcessedResult Process(const std::string& name)
		{
			const auto path = root / "assets/models" / (name + ".gltf");
			const auto bytes = aether::io::file_util::ReadBinary(path);
			REQUIRE(bytes.has_value());
			auto result = aether::assetpipeline::MeshProcessor::Process(*bytes, path, "assets/models/" + name + ".gltf", root);
			REQUIRE(result.error.empty());
			REQUIRE_FALSE(result.meshData.empty());
			return result;
		}

		bool LoadMesh(const aether::assetpipeline::ByteBuffer& bytes)
		{
			// Runtime decoding resolves companions through VFS. Use a fixture-owned
			// prefix without replacing project:// or shutting down shared suite state.
			if (!aether::io::FileSystem::IsInitialized())
			{
				aether::io::FileSystem::Initialize();
			}
			aether::io::FileSystem::Mount("model_bake_fixture", root);
			return aether::assets::GltfAsset::LoadFromMemory(bytes, "model_bake_fixture://assets/models/first.mesh").has_value();
		}
	};
}

TEST_CASE("Model bake cache repairs every missing companion and header-valid mesh truncation")
{
	using namespace aether;
	ModelBakeFixture fixture;
	const auto processed = fixture.Process("first");
	REQUIRE_FALSE(processed.skelData.empty());
	REQUIRE_FALSE(processed.animsetData.empty());
	REQUIRE_FALSE(processed.animFiles.empty());
	REQUIRE_FALSE(processed.materialFiles.empty());
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	CHECK(assetpipeline::BakeAllModels(fixture.root).upToDate == 1);
	for (const auto& path : {fixture.root / "assets/models/first.skel", fixture.root / "assets/models/first.animset",
	                         fixture.root / "animations" / processed.animFiles.front().first, fixture.root / processed.materialFiles.front().first})
	{
		REQUIRE(std::filesystem::remove(path));
		CHECK(assetpipeline::BakeAllModels(fixture.root).baked == 1);
		CHECK(std::filesystem::is_regular_file(path));
	}
	std::filesystem::resize_file(fixture.root / "assets/models/first.mesh", sizeof(MeshHeaderDisk));
	CHECK(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto repaired = io::file_util::ReadBinary(fixture.root / "assets/models/first.mesh");
	REQUIRE(repaired.has_value());
	CHECK(fixture.LoadMesh(*repaired));
}

TEST_CASE("Model bake cache detects source and dependency edits with preserved timestamps")
{
	using namespace aether;
	ModelBakeFixture fixture;
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto source = fixture.root / "assets/models/first.gltf";
	const auto sourceTime = std::filesystem::last_write_time(source);
	fixture.document["nodes"][0]["name"] = "Renamed";
	fixture.WriteModel("first");
	std::filesystem::last_write_time(source, sourceTime);
	CHECK(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto dependency = fixture.root / "assets/models/geometry.bin";
	const auto dependencyTime = std::filesystem::last_write_time(dependency);
	{
		std::fstream file(dependency, std::ios::binary | std::ios::in | std::ios::out);
		const float width = 2;
		file.seekp(12);
		file.write(reinterpret_cast<const char*>(&width), sizeof(width));
	}
	std::filesystem::last_write_time(dependency, dependencyTime);
	CHECK(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	std::filesystem::remove(dependency);
	CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 1);
}

TEST_CASE("Generated material conflicts reject before altering either model in both import orders")
{
	using namespace aether;
	for (const bool blueFirst : {false, true})
	{
		ModelBakeFixture fixture;
		if (blueFirst)
		{
			fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
			fixture.WriteModel("first");
		}
		const auto first = fixture.Process("first");
		REQUIRE(assetpipeline::WriteBakedOutputs(first, fixture.root / "assets/models/first.gltf", fixture.root).ok);
		const auto materialPath = fixture.root / first.materialFiles.front().first;
		const auto materialBefore = ReadBakeBytes(materialPath);
		fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = blueFirst ? nlohmann::json{1, 0, 0, 1} : nlohmann::json{0, 0, 1, 1};
		fixture.WriteModel("second");
		const auto second = fixture.Process("second");
		const auto write = assetpipeline::WriteBakedOutputs(second, fixture.root / "assets/models/second.gltf", fixture.root);
		CHECK_FALSE(write.ok);
		CHECK_FALSE(write.error.empty());
		CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/models/second.mesh"));
		CHECK(ReadBakeBytes(materialPath) == materialBefore);
		CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 1);
	}
}

TEST_CASE("Generated materials retain identical sharing and authored override paths")
{
	using namespace aether;
	ModelBakeFixture fixture;
	fixture.WriteModel("second");
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 2);
	CHECK(assetpipeline::BakeAllModels(fixture.root).upToDate == 2);
	const auto materialBefore = ReadBakeBytes(fixture.root / "assets/models/materials/Shared.material");
	fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
	fixture.WriteModel("first");
	CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 1);
	CHECK(ReadBakeBytes(fixture.root / "assets/models/materials/Shared.material") == materialBefore);
	std::filesystem::create_directories(fixture.root / "materials");
	std::ofstream(fixture.root / "materials/Shared.material.override.toml") << "[material]\nuvScroll = [0.055, 0.0]\n";
	const auto overridden = fixture.Process("first");
	CHECK(overridden.materialFiles.empty());
	CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 0);
	CHECK(ReadBakeBytes(fixture.root / "assets/models/materials/Shared.material") == materialBefore);
}

TEST_CASE("An exclusive generated material can be regenerated after its source color changes")
{
	using namespace aether;
	ModelBakeFixture fixture;
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto path = fixture.root / "assets/models/materials/Shared.material";
	const auto before = ReadBakeBytes(path);
	fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
	fixture.WriteModel("first");
	const auto rebuilt = assetpipeline::BakeAllModels(fixture.root);
	CHECK(rebuilt.baked == 1);
	CHECK(rebuilt.failed == 0);
	CHECK(ReadBakeBytes(path) != before);
}

TEST_CASE("Web 3D packing never skips a source behind an incomplete model cache")
{
	using namespace aether;
	ModelBakeFixture fixture;
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto baked = fixture.Process("first");
	std::filesystem::remove(fixture.root / "assets/models/first.skel");
	std::filesystem::resize_file(fixture.root / "assets/models/first.mesh", sizeof(MeshHeaderDisk));
	const auto packagePath = fixture.root.parent_path() / (fixture.root.filename().string() + ".pak");
	const auto packed = assetpipeline::PackDirectory(fixture.root, packagePath, {.target = "webgpu-3d-v1"});
	REQUIRE(packed.ok);
	const auto bytes = io::file_util::ReadBinary(packagePath);
	std::filesystem::remove(packagePath);
	REQUIRE(bytes.has_value());
	const auto package = io::MemoryPak::Open(*bytes, "webgpu-3d-v1");
	REQUIRE(package.has_value());
	CHECK(package->Read("assets/models/first.skel").has_value());
	const auto mesh = package->Read("assets/models/first.mesh");
	REQUIRE(mesh.has_value());
	CHECK(fixture.LoadMesh(*mesh));
}

TEST_CASE("Generated material ownership never claims legacy or hand edited bytes")
{
	using namespace aether;
	for (const bool legacy : {false, true})
	{
		ModelBakeFixture fixture;
		REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
		const auto meshPath = fixture.root / "assets/models/first.mesh";
		const auto materialPath = fixture.root / "assets/models/materials/Shared.material";
		if (legacy)
		{
			// Simulate upgrading an existing project whose generated bytes have no owner record.
			std::filesystem::remove_all(fixture.root / ".aether");
			REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
		}
		else
		{
			std::ofstream(materialPath, std::ios::binary | std::ios::app) << "authored edit";
		}
		const auto meshBefore = ReadBakeBytes(meshPath);
		const auto materialBefore = ReadBakeBytes(materialPath);
		fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
		fixture.WriteModel("first");
		const auto result = assetpipeline::BakeAllModels(fixture.root);
		CHECK(result.failed == 1);
		REQUIRE(result.failures.size() == 1);
		CHECK(result.failures.front().find("material collision") != std::string::npos);
		CHECK(ReadBakeBytes(meshPath) == meshBefore);
		CHECK(ReadBakeBytes(materialPath) == materialBefore);
	}
}

TEST_CASE("Failed model companion writes cannot certify a partial bake")
{
	using namespace aether;
	ModelBakeFixture fixture;
	const auto processed = fixture.Process("first");
	std::filesystem::create_directory(fixture.root / "assets/models/first.skel");
	const auto write = assetpipeline::WriteBakedOutputs(processed, fixture.root / "assets/models/first.gltf", fixture.root);
	CHECK_FALSE(write.ok);
	CHECK_FALSE(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
	std::filesystem::remove(fixture.root / "assets/models/first.skel");
	CHECK(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	CHECK(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
}

TEST_CASE("Model package incremental checks detect same timestamp geometry changes")
{
	using namespace aether;
	ModelBakeFixture fixture;
	const auto packagePath = fixture.root.parent_path() / (fixture.root.filename().string() + ".pak");
	REQUIRE(assetpipeline::PackDirectory(fixture.root, packagePath, {.target = "webgpu-2d-v1"}).ok);
	const auto before = ReadBakeBytes(packagePath);
	const auto dependency = fixture.root / "assets/models/geometry.bin";
	const auto time = std::filesystem::last_write_time(dependency);
	{
		std::fstream file(dependency, std::ios::binary | std::ios::in | std::ios::out);
		const float width = 2;
		file.seekp(12);
		file.write(reinterpret_cast<const char*>(&width), sizeof(width));
	}
	std::filesystem::last_write_time(dependency, time);
	REQUIRE(assetpipeline::PackDirectory(fixture.root, packagePath, {.target = "webgpu-2d-v1"}).ok);
	const auto after = ReadBakeBytes(packagePath);
	CHECK(after != before);
	for (const auto& suffix : {"", ".manifest", ".log"})
	{
		std::filesystem::remove(std::filesystem::path(packagePath.string() + suffix));
	}
}

TEST_CASE("Failed model companion replacement retains material ownership for retry")
{
	using namespace aether;
	ModelBakeFixture fixture;
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto materialPath = fixture.root / "assets/models/materials/Shared.material";
	const auto before = ReadBakeBytes(materialPath);
	fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
	fixture.WriteModel("first");
	const auto skeletonPath = fixture.root / "assets/models/first.skel";
	REQUIRE(std::filesystem::remove(skeletonPath));
	std::filesystem::create_directory(skeletonPath);
	CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 1);
	CHECK_FALSE(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
	CHECK(ReadBakeBytes(materialPath) == before);
	std::filesystem::remove(skeletonPath);
	const auto retry = assetpipeline::BakeAllModels(fixture.root);
	CHECK(retry.failed == 0);
	CHECK(retry.baked == 1);
	CHECK(ReadBakeBytes(materialPath) != before);
	CHECK(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
}

TEST_CASE("Model bake dependencies decode escaped external buffer URIs like cgltf")
{
	using namespace aether;
	ModelBakeFixture fixture;
	std::filesystem::rename(fixture.root / "assets/models/geometry.bin", fixture.root / "assets/models/mesh data.bin");
	fixture.document["buffers"][0]["uri"] = "mesh%20data.bin";
	fixture.WriteModel("first");
	REQUIRE_FALSE(fixture.Process("first").meshData.empty());
	const auto bake = assetpipeline::BakeAllModels(fixture.root);
	CHECK(bake.failed == 0);
	CHECK(bake.baked == 1);
	CHECK(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
	std::filesystem::remove(fixture.root / "assets/models/mesh data.bin");
	CHECK_FALSE(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
	CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 1);
}

TEST_CASE("Model bake journal retains ownership after partial multi material replacement")
{
	using namespace aether;
	ModelBakeFixture fixture;
	fixture.document["materials"].push_back(fixture.document["materials"][0]);
	fixture.document["materials"][1]["name"] = "Other";
	fixture.document["meshes"][0]["primitives"].push_back(fixture.document["meshes"][0]["primitives"][0]);
	fixture.document["meshes"][0]["primitives"][1]["material"] = 1;
	fixture.WriteModel("first");
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	const auto receipt = fixture.root / ".aether/model-bakes/assets/models/first.gltf.receipt";
	const auto pending = fixture.root / ".aether/model-bakes/pending/assets/models/first.gltf.receipt";
	const auto otherMaterial = fixture.root / "assets/models/materials/Other.material";
	const auto originalReceipt = ReadBakeBytes(receipt);
	const auto originalOther = ReadBakeBytes(otherMaterial);
	for (auto& material : fixture.document["materials"])
	{
		material["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
	}
	fixture.WriteModel("first");
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	// Recreate an interrupted promotion: first material has new bytes, second
	// retains old bytes, published receipt is old and pending journal is new.
	std::filesystem::create_directories(pending.parent_path());
	std::filesystem::copy_file(receipt, pending, std::filesystem::copy_options::overwrite_existing);
	std::ofstream(receipt, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(originalReceipt.data()), originalReceipt.size());
	std::ofstream(otherMaterial, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(originalOther.data()), originalOther.size());
	CHECK_FALSE(assetpipeline::HasCurrentModelBake(fixture.root / "assets/models/first.gltf", fixture.root));
	REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
	// Retrying must retain exclusive ownership of the already-written first
	// material, so another ordinary source edit remains regenerable.
	for (auto& material : fixture.document["materials"])
	{
		material["pbrMetallicRoughness"]["baseColorFactor"] = {0, 1, 0, 1};
	}
	fixture.WriteModel("first");
	CHECK(assetpipeline::BakeAllModels(fixture.root).baked == 1);
}

#ifdef _WIN32
TEST_CASE("Generated material ownership recognizes Windows case aliases shared by different models")
{
	using namespace aether;
	for (const bool lowerFirst : {false, true})
	{
		ModelBakeFixture fixture;
		fixture.document["materials"][0]["name"] = lowerFirst ? "shared" : "Shared";
		fixture.WriteModel("first");
		REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
		fixture.document["materials"][0]["name"] = lowerFirst ? "Shared" : "shared";
		fixture.WriteModel("second");
		REQUIRE(assetpipeline::BakeAllModels(fixture.root).baked == 1);
		const auto material = fixture.root / "assets/models/materials/Shared.material";
		const auto before = ReadBakeBytes(material);
		fixture.document["materials"][0]["name"] = lowerFirst ? "shared" : "Shared";
		fixture.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
		fixture.WriteModel("first");
		CHECK(assetpipeline::BakeAllModels(fixture.root).failed == 1);
		CHECK(ReadBakeBytes(material) == before);
	}
}

TEST_CASE("Generated material preflight rejects Windows case aliases within one model")
{
	using namespace aether;
	ModelBakeFixture fixture;
	fixture.document["materials"].push_back(fixture.document["materials"][0]);
	fixture.document["materials"][1]["name"] = "shared";
	fixture.document["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1, 1};
	fixture.WriteModel("first");
	const auto processed = fixture.Process("first");
	const auto write = assetpipeline::WriteBakedOutputs(processed, fixture.root / "assets/models/first.gltf", fixture.root);
	CHECK_FALSE(write.ok);
	CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/models/first.mesh"));
}
#endif

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
