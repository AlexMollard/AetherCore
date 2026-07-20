// Verifies that ReadPrefabFile caches parsed prefabs (so instantiating the same
// prefab thousands of times doesn't re-read and re-parse the file each spawn),
// and that the cache is refreshed on save and cleared when the project changes.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "io/FileUtil.hpp"
#include "scene/SceneSerializer.hpp"

using namespace aether::app::scene;

namespace
{
	SceneDescription MakePrefab(std::string nodeName)
	{
		SceneDescription prefab;
		prefab.name = "P";
		EntityRecord record;
		record.entityId = 1;
		record.name = std::move(nodeName);
		record.hasTransform = true;
		prefab.entities.push_back(std::move(record));
		return prefab;
	}
}

TEST_CASE("ReadPrefabFile caches parsed prefabs and invalidates correctly")
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_prefab_cache_test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	SetProjectSceneDirectories(dir / "scenes", dir);

	// Saving populates the cache.
	REQUIRE(SavePrefabFile("Widget", MakePrefab("Node")));

	const auto first = ReadPrefabFile("Widget");
	REQUIRE(first.has_value());
	REQUIRE(first->entities.size() == 1);
	CHECK(first->entities[0].name == "Node");

	// Corrupt the on-disk sources. A cached read must NOT touch them.
	// (SavePrefabFile cooks a .prefab.bin sibling, so corrupt both forms.)
	REQUIRE(aether::io::file_util::WriteText(dir / "Widget.prefab.toml", "this is not valid toml {{{").has_value());
	std::filesystem::remove(dir / "Widget.prefab.bin");

	const auto cached = ReadPrefabFile("Widget");
	REQUIRE(cached.has_value());
	CHECK(cached->entities.size() == 1);
	CHECK(cached->entities[0].name == "Node"); // served from cache, not the garbage file

	// Switching the project's prefab directory clears the cache, forcing a re-read;
	// with both sources gone/garbage, the re-read must fail - proving it re-read.
	ClearProjectSceneDirectories();
	SetProjectSceneDirectories(dir / "scenes", dir);
	const auto reread = ReadPrefabFile("Widget");
	CHECK((!reread.has_value() || reread->entities.empty()));

	ClearProjectSceneDirectories();
	std::filesystem::remove_all(dir);
}
