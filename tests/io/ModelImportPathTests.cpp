#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "editor/ModelImportPath.hpp"

TEST_CASE("Model import normalizes project-contained absolute paths before VFS access")
{
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() / fs::u8path(u8"Aether project \u03a9");
	const auto model = root / fs::u8path(u8"assets/models/triangle \u65e5.glb");
	const auto bytes = model.generic_u8string();
	std::string normalized;
	std::string error;
	REQUIRE(aether::editor::NormalizeModelImportPath(std::string(bytes.begin(), bytes.end()), root, normalized, error));
	const std::u8string expected = u8"project://assets/models/triangle \u65e5.glb";
	CHECK(normalized == std::string(expected.begin(), expected.end()));
	REQUIRE(aether::editor::NormalizeModelImportPath("project://assets/../models/triangle.glb", root, normalized, error));
	CHECK(normalized == "project://models/triangle.glb");
}

TEST_CASE("Model import rejects traversal and sibling-prefix absolute paths")
{
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() / "Aether-import-project";
	std::string normalized;
	std::string error;
	for (const auto& input : {std::string("project://../outside.glb"), std::string("project://assets/../../outside.glb"), (root.parent_path() / "Aether-import-project-sibling" / "model.glb").string(), root.string()})
	{
		CHECK_FALSE(aether::editor::NormalizeModelImportPath(input, root, normalized, error));
		CHECK(error.find("outside the open project") != std::string::npos);
	}
	CHECK_FALSE(aether::editor::NormalizeModelImportPath("assets/model.glb", root, normalized, error));
	CHECK_FALSE(aether::editor::NormalizeModelImportPath((root / "model.glb").string(), {}, normalized, error));
	REQUIRE(aether::editor::NormalizeModelImportPath("uid://1234", {}, normalized, error));
	CHECK(normalized == "uid://1234");
}
