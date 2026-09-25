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

	std::string DecodeShaderVfsPath(const std::vector<std::byte>& blob)
	{
		BinaryReader reader(blob);
		const auto hdr = reader.Read<MaterialHeaderDisk>();
		REQUIRE(CheckMagic(hdr));

		for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
		{
			reader.Read<uint8_t>();
			reader.ReadString();
		}

		return reader.ReadString();
	}

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

// override was never read, so a mesh material's custom shader silently rendered
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
		CHECK(decoded.shaderVfsPath.empty());
	}
}

// integers are idiomatic TOML for whole values; as_floating_point() alone silently dropped
// them and baked the header defaults (roughnessFactor = 0 came out as 1.0)
TEST_CASE("MaterialProcessor accepts TOML integers for numeric material factors")
{
	const std::string toml =
	        "[material]\n"
	        "baseColorFactor = [1, 1, 1, 1]\n"
	        "metallicFactor = 0\n"
	        "roughnessFactor = 0\n"
	        "alphaCutoff = 0\n";

	const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "materials/int.material/properties.toml", "materials");
	REQUIRE_FALSE(blob.empty());

	BinaryReader reader(blob);
	const auto hdr = reader.Read<MaterialHeaderDisk>();
	REQUIRE(CheckMagic(hdr));
	CHECK(hdr.metallicFactor == 0.f);
	CHECK(hdr.roughnessFactor == 0.f);
	CHECK(hdr.alphaCutoff == 0.f);
	for (const float c: hdr.baseColorFactor)
	{
		CHECK(c == 1.f);
	}
}

// uvScroll was carved from the old header's _pad[11] (size still 64): a header written
// before the field has zeros there, and zeros mean a static texture - exactly how those
// files rendered. Covers both the bake of the new key and the read of an old blob.
TEST_CASE("MaterialProcessor bakes uvScroll and old headers still read as static")
{
	SUBCASE("properties.toml uvScroll lands in the header")
	{
		const std::string toml =
		        "[material]\n"
		        "uvScroll = [0.3, 0.03]\n";

		const assetpipeline::ByteBuffer blob = assetpipeline::MaterialProcessor::Process(ToBytes(toml), "materials/water.material/properties.toml", "materials");
		REQUIRE_FALSE(blob.empty());

		BinaryReader reader(blob);
		const auto hdr = reader.Read<MaterialHeaderDisk>();
		REQUIRE(CheckMagic(hdr));
		CHECK(hdr.uvScroll[0] == doctest::Approx(0.3f));
		CHECK(hdr.uvScroll[1] == doctest::Approx(0.03f));
	}

	SUBCASE("a pre-uvScroll header's pad zeros read as no scroll")
	{
		// 64 bytes of a plausible old header: real magic/version, everything else zero.
		std::vector<std::byte> old(sizeof(MaterialHeaderDisk));
		MaterialHeaderDisk hdr; // writes current magic/version, uvScroll zeroed too
		hdr.uvScroll[0] = 0.f;
		hdr.uvScroll[1] = 0.f;
		std::memcpy(old.data(), &hdr, sizeof(hdr));

		BinaryReader reader(old);
		const auto read = reader.Read<MaterialHeaderDisk>();
		REQUIRE(CheckMagic(read));
		CHECK(read.uvScroll[0] == 0.f);
		CHECK(read.uvScroll[1] == 0.f);
	}
}
