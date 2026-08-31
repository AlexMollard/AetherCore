#include <doctest/doctest.h>

#include "material/MaterialSerializer.hpp"

using namespace aether;

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
