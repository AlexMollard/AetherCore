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

	// A cached read must not touch the sources while the file is unchanged. Proving that
	// takes corrupting the file and putting its mtime back: the cache keys on the source
	// mtime, so an unchanged stamp must serve the parse rather than re-read the garbage.
	// (SavePrefabFile cooks a .prefab.bin sibling, so corrupt both forms.)
	const std::filesystem::path source = dir / "Widget.prefab.toml";
	const auto originalStamp = std::filesystem::last_write_time(source);
	REQUIRE(aether::io::file_util::WriteText(source, "this is not valid toml {{{").has_value());
	std::filesystem::remove(dir / "Widget.prefab.bin");
	std::filesystem::last_write_time(source, originalStamp);

	const auto cached = ReadPrefabFile("Widget");
	REQUIRE(cached.has_value());
	CHECK(cached->entities.size() == 1);
	CHECK(cached->entities[0].name == "Node"); // served from cache, not the garbage file

	// Now let the edit show its real mtime. A prefab edited behind the editor's back - a
	// git checkout, a merge, a hand-edit - must be re-read, not served from the cache
	// until restart; the file is garbage now, so the re-read must fail.
	std::filesystem::last_write_time(source, originalStamp + std::chrono::seconds(2));
	const auto afterEdit = ReadPrefabFile("Widget");
	CHECK((!afterEdit.has_value() || afterEdit->entities.empty()));

	// Switching the project's prefab directory clears the cache, forcing a re-read;
	// with both sources gone/garbage, the re-read must fail - proving it re-read.
	ClearProjectSceneDirectories();
	SetProjectSceneDirectories(dir / "scenes", dir);
	const auto reread = ReadPrefabFile("Widget");
	CHECK((!reread.has_value() || reread->entities.empty()));

	ClearProjectSceneDirectories();
	std::filesystem::remove_all(dir);
}
