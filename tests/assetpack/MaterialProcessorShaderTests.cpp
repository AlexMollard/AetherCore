#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <BinaryFormats.hpp>

#include "MaterialProcessor.hpp"
#include "utils/BinaryReader.hpp"

using namespace aether;

namespace
{
	std::vector<std::byte> ToBytes(const std::string& s)
	{
		const auto* p = reinterpret_cast<const std::byte*>(s.data());
		return std::vector<std::byte>(p, p + s.size());
	}

	// Decodes a baked .material blob using the EXACT BinaryReader sequence both
	// production readers use -- AssetManager::LoadMaterialPreset's binary branch
	// (src/engine/assets/AssetManager.cpp) and GltfAsset.cpp's LoadMaterialBinary
	// -- so this helper locks the writer (MaterialProcessor::Process) and both
	// readers into lockstep: fixed MaterialHeaderDisk, then texturePathCount
	// entries of {type, length-prefixed path}, then the trailing optional
	// length-prefixed shader path. Only the shader path is surfaced here; the
	// texture entries are walked (not resolved) purely to advance the cursor to
	// the shader field, exactly as both readers do before their own trailing
	// ReadString(). Both readers now consume the shader field (added to
	// LoadMaterialBinary for the mesh-embedded-material path).
	std::string DecodeShaderVfsPath(const std::vector<std::byte>& blob)
	{
		BinaryReader reader(blob);
		const auto hdr = reader.Read<MaterialHeaderDisk>();
		REQUIRE(CheckMagic(hdr));

		for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
		{
			reader.Read<uint8_t>(); // texture type
			reader.ReadString();    // texture path
		}

		return reader.ReadString();
	}

	// Fully mirrors GltfAsset.cpp's LoadMaterialBinary decode (all header fields
	// + every texture path + the trailing shader override), returning the fields
	// a mesh-embedded material carries into MaterialAsset. Proves the runtime
	// mesh-load decode reads the writer's output at the correct offset -- the gap
	// that let a mesh material's custom shader silently fall back to the default.
	struct DecodedMaterial
	{
		int textureCount = 0;
		std::string albedoPath;
		std::string shaderVfsPath;
	};

	DecodedMaterial DecodeLikeLoadMaterialBinary(const std::vector<std::byte>& blob)
	{
		DecodedMaterial out;
		BinaryReader reader(blob);
		const auto hdr = reader.Read<MaterialHeaderDisk>();
		REQUIRE(CheckMagic(hdr));
		out.textureCount = hdr.texturePathCount;

		for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
		{
			const auto type = reader.Read<uint8_t>();
			std::string texPath = reader.ReadString();
			if (static_cast<TextureTypeDisk>(type) == TextureTypeDisk::BaseColor)
			{
				out.albedoPath = std::move(texPath);
			}
		}

		out.shaderVfsPath = reader.ReadString();
		return out;
	}
}

TEST_CASE("MaterialProcessor bakes an optional shader override into the material blob")
{
	const std::string toml =
	        "[material]\n"
	        "baseColorFactor = [1.0, 1.0, 1.0, 1.0]\n"
	        "shader = \"shaders://custom.spv\"\n";

	const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "materials/custom.material/properties.toml", "materials");
	REQUIRE_FALSE(blob.empty());

	CHECK(DecodeShaderVfsPath(blob) == "shaders://custom.spv");
}

TEST_CASE("MaterialProcessor leaves the shader override empty when properties.toml omits it")
{
	const std::string toml =
	        "[material]\n"
	        "baseColorFactor = [1.0, 1.0, 1.0, 1.0]\n"
	        "doubleSided = true\n";

	const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "materials/plain.material/properties.toml", "materials");
	REQUIRE_FALSE(blob.empty());

	// Empty override -> AssetManager::LoadMaterialPreset's binary branch skips
	// assigning templateDesc.shaderVfsPath, so the MaterialAsset default
	// ("shaders://gltf_mesh.spv", set in MaterialAsset.hpp) is kept.
	CHECK(DecodeShaderVfsPath(blob).empty());
}

TEST_CASE("MaterialProcessor round-trips a shader override alongside texture entries")
{
	const std::string toml =
	        "[material]\n"
	        "shader = \"shaders://testcustom.spv\"\n"
	        "\n"
	        "[textures]\n"
	        "albedo = \"albedo.png\"\n"
	        "normal = \"normal.png\"\n";

	const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "materials/withtex.material/properties.toml", "materials");
	REQUIRE_FALSE(blob.empty());

	BinaryReader reader(blob);
	const auto hdr = reader.Read<MaterialHeaderDisk>();
	REQUIRE(CheckMagic(hdr));
	CHECK(hdr.texturePathCount == 2);

	CHECK(DecodeShaderVfsPath(blob) == "shaders://testcustom.spv");
}

// Load-path coverage for the mesh-embedded-material case (GltfAsset.cpp's
// LoadMaterialBinary -> FinaliseModelLoad -> MaterialTemplate::shaderVfsPath).
// LoadMaterialBinary lives in an anonymous namespace and reads through the VFS,
// so we can't call it directly; instead we drive the identical BinaryReader
// sequence over a real MaterialProcessor::Process blob (the same bytes a packed
// mesh material carries) and assert the shader field decodes at the right
// offset. Before this fix the reader stopped after the texture entries and the
// override was never read, so a mesh material's custom shader silently rendered
// with the engine default.
TEST_CASE("A mesh-embedded material's shader field decodes through the LoadMaterialBinary sequence")
{
	SUBCASE("present -> the custom shader path is read")
	{
		const std::string toml =
		        "[material]\n"
		        "metallicFactor = 0.25\n"
		        "shader = \"shaders://testcustom.spv\"\n"
		        "\n"
		        "[textures]\n"
		        "albedo = \"grass_color.png\"\n";

		const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "models/Fox/materials/fox.material/properties.toml", "models/Fox");
		REQUIRE_FALSE(blob.empty());

		const DecodedMaterial decoded = DecodeLikeLoadMaterialBinary(blob);
		CHECK(decoded.textureCount == 1);
		CHECK(decoded.albedoPath == "grass_color.png");
		// This is the value FinaliseModelLoad interns into
		// MaterialAsset.templateDesc.shaderVfsPath for the spawned mesh.
		CHECK(decoded.shaderVfsPath == "shaders://testcustom.spv");
	}

	SUBCASE("absent -> empty, so FinaliseModelLoad keeps the MaterialAsset default")
	{
		const std::string toml =
		        "[material]\n"
		        "metallicFactor = 0.25\n"
		        "\n"
		        "[textures]\n"
		        "albedo = \"grass_color.png\"\n";

		const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "models/Fox/materials/fox.material/properties.toml", "models/Fox");
		REQUIRE_FALSE(blob.empty());

		const DecodedMaterial decoded = DecodeLikeLoadMaterialBinary(blob);
		CHECK(decoded.textureCount == 1);
		// Literal empty string (not a truncated/garbage read): the length-prefix
		// is present and zero, so ReadString() returns "". FinaliseModelLoad's
		// `if (!srcMat.shaderVfsPath.empty())` guard then leaves the default
		// ("shaders://gltf_mesh.spv") in place.
		CHECK(decoded.shaderVfsPath.empty());
	}
}
