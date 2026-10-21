#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

#include <BinaryFormats.hpp>

#include "material/MaterialSerializer.hpp"
#include "material/MaterialOverrides.hpp"
#include "io/FileSystem.hpp"

using namespace aether;

TEST_CASE("Linked material fields distinguish inherited and explicitly cleared textures")
{
	const std::array<std::string, 2> keys{"albedo", "base_color"};
	CHECK(IsMaterialFieldOverridden(keys, "albedo"));
	CHECK_FALSE(IsMaterialFieldOverridden(keys, "emissive_tex"));
	CHECK_FALSE(IsMaterialFieldOverridden({}, "albedo"));
	MaterialAsset asset;
	asset.roughnessFactor = 0.2f;
	MaterialAsset override;
	override.baseColorFactor = {0.2f, 0.3f, 0.4f, 0.5f};
	ApplyMaterialOverrides(asset, override, keys);
	CHECK(asset.baseColorFactor == override.baseColorFactor);
	CHECK(asset.roughnessFactor == doctest::Approx(0.2f));
}

TEST_CASE("Specialized and additive material flags survive authored round trips")
{
	constexpr bool MaterialAsset::* flags[] = {
	        &MaterialAsset::additiveBlend, &MaterialAsset::foliage, &MaterialAsset::bakedLighting,
	        &MaterialAsset::sky, &MaterialAsset::lightShaft, &MaterialAsset::objectLit,
	        &MaterialAsset::gem, &MaterialAsset::sheen, &MaterialAsset::water, &MaterialAsset::character};
	MaterialPresetSpec source;
	source.material.alphaBlend = true;
	for (const auto field: flags) { source.material.*field = true; }
	const auto parsed = MaterialSerializer::Parse("project://flags.material.toml", MaterialSerializer::ToToml(source));
	for (const auto field: flags) { CHECK(parsed.material.*field); }
}

namespace
{
	MaterialPresetSpec MakeNonDefaultSpec()
	{
		// Every value deliberately differs from the struct's default, so a field the writer
		// forgets to emit shows up as the default coming back rather than silently matching.
		MaterialPresetSpec spec;
		spec.material.baseColorFactor = glm::vec4(0.25f, 0.5f, 0.75f, 0.5f);
		spec.material.emissiveFactor = glm::vec3(1.5f, 0.25f, 3.0f);
		spec.material.metallicFactor = 0.875f;
		spec.material.roughnessFactor = 0.125f;
		spec.material.occlusionStrength = 0.625f;
		spec.material.alphaCutoff = 0.375f;
		spec.material.uvScroll = glm::vec2(0.3f, 0.03f);
		spec.material.doubleSided = true;
		spec.material.alphaBlend = true;
		spec.material.alphaMask = true;
		spec.material.modulateVertexColor = true;
		spec.material.receiveShadows = false;
		spec.albedoPath = "project://assets/textures/rock_albedo.png";
		spec.normalPath = "project://assets/textures/rock_normal.png";
		spec.metallicRoughnessPath = "project://assets/textures/rock_mr.png";
		spec.occlusionPath = "project://assets/textures/rock_ao.png";
		spec.emissivePath = "project://assets/textures/rock_emissive.png";
		spec.shaderVfsPath = "shaders://gltf_mesh.spv";
		return spec;
	}
} // namespace

TEST_CASE("A material survives a TOML round trip unchanged")
{
	const MaterialPresetSpec original = MakeNonDefaultSpec();
	const MaterialPresetSpec parsed = MaterialSerializer::Parse("project://assets/rock.material.toml", MaterialSerializer::ToToml(original));

	CHECK(parsed.material.baseColorFactor.x == doctest::Approx(original.material.baseColorFactor.x));
	CHECK(parsed.material.baseColorFactor.y == doctest::Approx(original.material.baseColorFactor.y));
	CHECK(parsed.material.baseColorFactor.z == doctest::Approx(original.material.baseColorFactor.z));
	CHECK(parsed.material.baseColorFactor.w == doctest::Approx(original.material.baseColorFactor.w));
	CHECK(parsed.material.emissiveFactor.x == doctest::Approx(original.material.emissiveFactor.x));
	CHECK(parsed.material.emissiveFactor.y == doctest::Approx(original.material.emissiveFactor.y));
	CHECK(parsed.material.emissiveFactor.z == doctest::Approx(original.material.emissiveFactor.z));
	CHECK(parsed.material.metallicFactor == doctest::Approx(original.material.metallicFactor));
	CHECK(parsed.material.roughnessFactor == doctest::Approx(original.material.roughnessFactor));
	CHECK(parsed.material.occlusionStrength == doctest::Approx(original.material.occlusionStrength));
	CHECK(parsed.material.alphaCutoff == doctest::Approx(original.material.alphaCutoff));
	CHECK(parsed.material.uvScroll.x == doctest::Approx(original.material.uvScroll.x));
	CHECK(parsed.material.uvScroll.y == doctest::Approx(original.material.uvScroll.y));
	CHECK(parsed.material.doubleSided == original.material.doubleSided);
	CHECK(parsed.material.alphaBlend == original.material.alphaBlend);
	CHECK(parsed.material.alphaMask == original.material.alphaMask);
	CHECK(parsed.shaderVfsPath == original.shaderVfsPath);
}

// These two are the reason the reader and writer now live in one file. The inspector could
// always edit them, but the format had no key for either, so saving a material and loading it
// back silently reset both to their defaults.
TEST_CASE("Vertex-colour modulation and shadow receipt survive the round trip")
{
	const MaterialPresetSpec original = MakeNonDefaultSpec();
	const MaterialPresetSpec parsed = MaterialSerializer::Parse("project://assets/rock.material.toml", MaterialSerializer::ToToml(original));

	CHECK(parsed.material.modulateVertexColor == true);
	CHECK(parsed.material.receiveShadows == false);
}

TEST_CASE("Texture slots round trip by path")
{
	const MaterialPresetSpec original = MakeNonDefaultSpec();
	const MaterialPresetSpec parsed = MaterialSerializer::Parse("project://assets/rock.material.toml", MaterialSerializer::ToToml(original));

	CHECK(parsed.albedoPath == original.albedoPath);
	CHECK(parsed.normalPath == original.normalPath);
	CHECK(parsed.metallicRoughnessPath == original.metallicRoughnessPath);
	CHECK(parsed.occlusionPath == original.occlusionPath);
	CHECK(parsed.emissivePath == original.emissivePath);
}

TEST_CASE("An empty texture slot is omitted rather than written blank")
{
	// A blank value would resolve against the material's own directory and come back as a
	// path to the folder, which then fails to load as an image.
	MaterialPresetSpec spec;
	spec.albedoPath = "project://assets/textures/only_albedo.png";
	const std::string toml = MaterialSerializer::ToToml(spec);
	CHECK(toml.find("normal") == std::string::npos);

	const MaterialPresetSpec parsed = MaterialSerializer::Parse("project://assets/x.material.toml", toml);
	CHECK(parsed.albedoPath == spec.albedoPath);
	CHECK(parsed.normalPath.empty());
	CHECK(parsed.emissivePath.empty());
}

TEST_CASE("A texture path written relative to the material resolves against it")
{
	const MaterialPresetSpec parsed = MaterialSerializer::Parse("project://assets/materials/rock.material.toml",
	        "[textures]\nalbedo = 'textures/rock.png'\n");
	CHECK(parsed.albedoPath == "project://assets/materials/textures/rock.png");
}

namespace
{
	void EnsureMaterialMount()
	{
		if (!io::FileSystem::IsInitialized()) { io::FileSystem::Initialize(); }
		if (!io::FileSystem::IsMounted("material_loader_tests"))
		{
			const auto root = std::filesystem::temp_directory_path() / ("aether_material_loader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root / "folder");
			std::filesystem::create_directories(root / "published");
			io::FileSystem::Mount("material_loader_tests", root);
		}
	}

	void WriteMaterialText(std::string_view name, std::string_view text)
	{
		EnsureMaterialMount();
		REQUIRE(io::FileSystem::WriteFileText("material_loader_tests://" + std::string(name), text).has_value());
	}

	void WriteMaterialBytes(std::span<const std::byte> bytes)
	{
		EnsureMaterialMount();
		REQUIRE(io::FileSystem::WriteFile("material_loader_tests://cooked.material", bytes).has_value());
	}

	std::vector<std::byte> CookedMaterial(std::uint8_t type = static_cast<std::uint8_t>(TextureTypeDisk::BaseColor), std::string_view texture = "maps/base.texture")
	{
		MaterialHeaderDisk header;
		header.baseColorFactor[0] = 0.25f;
		header.metallicFactor = 0.3f;
		header.roughnessFactor = 0.7f;
		header.emissiveFactor[1] = 2.5f;
		header.alphaCutoff = 0.4f;
		header.doubleSided = header.alphaMask = header.modulateVertexColor = header.foliage = header.objectLit = 1;
		header.alphaBlend = 2;
		header.bakedLighting = kMaterialDiskBaked | kMaterialDiskSky | kMaterialDiskLightShaft | kMaterialDiskGem | kMaterialDiskSheen | kMaterialDiskWater | kMaterialDiskCharacter;
		header.uvScroll[0] = 0.125f;
		header.uvScroll[1] = -0.25f;
		header.texturePathCount = 1;
		std::vector<std::byte> bytes(sizeof(header));
		std::memcpy(bytes.data(), &header, sizeof(header));
		bytes.push_back(static_cast<std::byte>(type));
		auto appendString = [&](std::string_view text)
		{
			const auto size = static_cast<std::uint16_t>(text.size());
			bytes.push_back(static_cast<std::byte>(size & 0xff));
			bytes.push_back(static_cast<std::byte>(size >> 8));
			for (const char character: text) { bytes.push_back(static_cast<std::byte>(character)); }
		};
		appendString(texture);
		appendString("shaders://custom.spv");
		return bytes;
	}
} // namespace

TEST_CASE("Material loader resolves folder aliases with native extension and override precedence")
{
	WriteMaterialText("folder/properties.toml", "[material]\nroughnessfactor = 0.3\n[textures]\nnormal = 'explicit.texture'\n");
	for (const auto* name: {"albedo.png", "albedo.texture", "basecolor.texture", "normal.texture", "orm.texture", "ao.texture", "emission.texture"})
	{
		WriteMaterialText("folder/" + std::string(name), "fixture");
	}
	const auto result = MaterialSerializer::Load("material_loader_tests://folder/");
	REQUIRE(result.has_value());
	CHECK(result->material.roughnessFactor == doctest::Approx(0.3f));
	CHECK(result->albedoPath == "material_loader_tests://folder/albedo.texture");
	CHECK(result->normalPath == "material_loader_tests://folder/explicit.texture");
	CHECK(result->metallicRoughnessPath == "material_loader_tests://folder/orm.texture");
	CHECK(result->occlusionPath == "material_loader_tests://folder/ao.texture");
	CHECK(result->emissivePath == "material_loader_tests://folder/emission.texture");
}

TEST_CASE("Material loader reads authored files and reports malformed and missing presets")
{
	WriteMaterialText("authored.material.toml", "[material]\nroughnessfactor = 0.6\n[textures]\nalbedo = 'maps/color.texture'\n");
	const auto result = MaterialSerializer::Load("material_loader_tests://authored.material.toml");
	REQUIRE(result.has_value());
	CHECK(result->material.roughnessFactor == doctest::Approx(0.6f));
	CHECK(result->albedoPath == "material_loader_tests://maps/color.texture");
	WriteMaterialText("broken.material.toml", "[material\n");
	CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://broken.material.toml").has_value());
	CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://missing.material.toml").has_value());
}

TEST_CASE("Material loader resolves published folder presets and their sibling cooked maps")
{
	EnsureMaterialMount();
	MaterialHeaderDisk header;
	header.roughnessFactor = 0.45f;
	std::vector<std::byte> bytes(sizeof(header) + sizeof(std::uint16_t));
	std::memcpy(bytes.data(), &header, sizeof(header));
	REQUIRE(io::FileSystem::WriteFile("material_loader_tests://published.material", bytes).has_value());
	REQUIRE(io::FileSystem::WriteFileText("material_loader_tests://published/albedo.texture", "cooked texture fixture").has_value());
	const auto result = MaterialSerializer::Load("material_loader_tests://published/");
	REQUIRE(result.has_value());
	CHECK(result->material.roughnessFactor == doctest::Approx(0.45f));
	CHECK(result->albedoPath == "material_loader_tests://published/albedo.texture");
	CHECK(result->normalPath.empty());
}

TEST_CASE("Material loader preserves cooked header flags, slot types and optional shader trailer")
{
	const auto bytes = CookedMaterial();
	WriteMaterialBytes(bytes);
	const auto result = MaterialSerializer::Load("material_loader_tests://cooked.material");
	REQUIRE(result.has_value());
	const auto& material = result->material;
	CHECK(material.baseColorFactor.x == doctest::Approx(0.25f));
	CHECK(material.metallicFactor == doctest::Approx(0.3f));
	CHECK(material.roughnessFactor == doctest::Approx(0.7f));
	CHECK(material.emissiveFactor.y == doctest::Approx(2.5f));
	CHECK(material.alphaCutoff == doctest::Approx(0.4f));
	CHECK(material.uvScroll == glm::vec2(0.125f, -0.25f));
	CHECK(material.doubleSided);
	CHECK(material.alphaBlend);
	CHECK(material.additiveBlend);
	CHECK(material.alphaMask);
	CHECK(material.modulateVertexColor);
	CHECK(material.foliage);
	CHECK(material.bakedLighting);
	CHECK(material.sky);
	CHECK(material.lightShaft);
	CHECK(material.gem);
	CHECK(material.sheen);
	CHECK(material.water);
	CHECK(material.character);
	CHECK(material.objectLit);
	CHECK(result->albedoPath == "material_loader_tests://maps/base.texture");
	CHECK(result->shaderVfsPath == "shaders://custom.spv");
	const std::string MaterialPresetSpec::* slots[] = {&MaterialPresetSpec::albedoPath, &MaterialPresetSpec::metallicRoughnessPath, &MaterialPresetSpec::normalPath, &MaterialPresetSpec::occlusionPath, &MaterialPresetSpec::emissivePath};
	for (std::uint8_t type = 0; type < 5; ++type)
	{
		auto legacy = CookedMaterial(type);
		legacy.resize(legacy.size() - 2 - std::string_view("shaders://custom.spv").size());
		WriteMaterialBytes(legacy);
		const auto loaded = MaterialSerializer::Load("material_loader_tests://cooked.material");
		REQUIRE(loaded.has_value());
		CHECK((*loaded).*slots[type] == "material_loader_tests://maps/base.texture");
		CHECK(loaded->shaderVfsPath.empty());
	}
}

TEST_CASE("Material loader rejects truncated cooked fields rather than inventing defaults")
{
	const auto bytes = CookedMaterial();
	const auto legacyEnd = sizeof(MaterialHeaderDisk) + 3 + std::string_view("maps/base.texture").size();
	for (std::size_t size = 0; size < bytes.size(); ++size)
	{
		if (size == legacyEnd) { continue; }
		CAPTURE(size);
		WriteMaterialBytes(std::span(bytes).first(size));
		CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://cooked.material").has_value());
	}
	for (const auto offset: {std::size_t{0}, std::size_t{4}})
	{
		auto invalid = bytes;
		invalid[offset] = std::byte{0xff};
		WriteMaterialBytes(invalid);
		CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://cooked.material").has_value());
	}
	WriteMaterialBytes(CookedMaterial(255));
	CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://cooked.material").has_value());
}

TEST_CASE("Material loader rejects resource paths that escape the VFS or contain embedded nul")
{
	for (const auto reference: {std::string("../../outside.texture"), std::string("project://../outside.texture"), std::string("bad\0name.texture", 16)})
	{
		WriteMaterialBytes(CookedMaterial(0, reference));
		CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://cooked.material").has_value());
	}
	WriteMaterialText("escape.material.toml", "[textures]\nalbedo = '../../outside.texture'\n");
	CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://escape.material.toml").has_value());
	CHECK_FALSE(MaterialSerializer::Load("material_loader_tests://../outside.material").has_value());
}
