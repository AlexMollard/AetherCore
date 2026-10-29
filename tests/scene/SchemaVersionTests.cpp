#include <doctest/doctest.h>

#include <string>

#include "scene/SceneSerializer.hpp"
#include "material/MaterialSerializer.hpp"

using namespace aether;
using namespace aether::app::scene;

TEST_CASE("Scene upgrades reject unsupported and malformed schemas before consuming records")
{
	for (const std::string version: {"999", "0", "-1", "1.5", "'16'", "true"})
	{
		CAPTURE(version);
		CHECK_FALSE(ParseToml("[scene]\nversion = " + version + "\nname = 'Untouched'\n[[entities]]\nname = 'Keep'\n").has_value());
	}
}

TEST_CASE("Prefab and component schemas reject future data without changing legacy meaning")
{
	CHECK_FALSE(ParseToml("[prefab]\nversion = 999\n[[entities]]\nname = 'Keep'\n").has_value());
	CHECK_FALSE(ParseToml("[[entities]]\nname = 'Keep'\n[entities.camera]\nschema_version = 999\nmain = true\n").has_value());
	CHECK_FALSE(ParseToml("[[entities]]\nname = 'Keep'\n[entities.camera]\nschema_version = '1'\n").has_value());
	const auto legacy = ParseToml("[[entities]]\nname = 'Keep'\n[entities.camera]\nprojection = 'orthographic'\nmain = true\n");
	REQUIRE(legacy.has_value());
	REQUIRE(legacy->entities.size() == 1);
	REQUIRE(legacy->entities[0].camera.has_value());
	CHECK(legacy->entities[0].mainCamera);
	const auto restored = ParseToml(WriteToml(*legacy, false));
	REQUIRE(restored.has_value());
	REQUIRE(restored->entities.size() == 1);
	CHECK(restored->entities[0].name == "Keep");
	REQUIRE(restored->entities[0].camera.has_value());
	CHECK(restored->entities[0].mainCamera);
	const auto binary = ReadSceneBinary(WriteSceneBinary(*legacy, false));
	REQUIRE(binary.has_value());
	REQUIRE(binary->entities.size() == 1);
	CHECK(binary->entities[0].name == "Keep");
	REQUIRE(binary->entities[0].camera.has_value());
	CHECK(binary->entities[0].mainCamera);
}

TEST_CASE("Material upgrades preserve legacy properties and reject future schemas")
{
	bool ok = false;
	const auto legacy = MaterialSerializer::Parse("project://assets/test.material.toml", "[material]\nroughnessfactor = 0.37\n", &ok);
	REQUIRE(ok);
	CHECK(legacy.material.roughnessFactor == doctest::Approx(0.37f));
	const auto restored = MaterialSerializer::Parse("project://assets/test.material.toml", MaterialSerializer::ToToml(legacy), &ok);
	REQUIRE(ok);
	CHECK(restored.material.roughnessFactor == doctest::Approx(0.37f));
	for (const std::string version: {"999", "0", "1.25", "'1'"})
	{
		CAPTURE(version);
		(void) MaterialSerializer::Parse("project://assets/test.material.toml", "[material]\nschema_version = " + version + "\n", &ok);
		CHECK_FALSE(ok);
	}
}
