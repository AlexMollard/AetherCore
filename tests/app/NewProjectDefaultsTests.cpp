// Where the "New Project" dialog proposes to put things. The bug these pin: Location is
// the PARENT folder and ComposeNewProjectRoot appends the name, but the default was seeded
// with parent/name - so accepting every default created AetherProject/AetherProject inside
// whatever the working directory happened to be (the engine build tree, or an unwritable
// install directory).
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "project/ProjectPaths.hpp"

using aether::app::project::ComposeNewProjectRoot;
using aether::app::project::DefaultNewProjectParent;

namespace
{
	std::filesystem::path MakeTempDir(const char* stem)
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether-newproject-tests" / stem;
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		std::filesystem::create_directories(dir, ec);
		return dir;
	}
} // namespace

TEST_CASE("DefaultNewProjectParent returns the folder that HOLDS the newest project, not the project")
{
	const std::filesystem::path root = MakeTempDir("holds");
	const std::filesystem::path projects = root / "MyProjects";
	std::error_code ec;
	std::filesystem::create_directories(projects / "Platformer", ec);

	const std::vector<std::filesystem::path> recents{projects / "Platformer"};
	const std::filesystem::path parent = DefaultNewProjectParent(recents, root / "Documents");

	CHECK(parent == (projects).lexically_normal());
	// The whole point: composing on top of it adds the name exactly once.
	CHECK(ComposeNewProjectRoot(parent, "Shooter") == (projects / "Shooter").lexically_normal());
}

TEST_CASE("DefaultNewProjectParent composing the default name does not nest it twice")
{
	const std::filesystem::path root = MakeTempDir("nesting");
	const std::filesystem::path documents = root / "Documents";
	std::error_code ec;
	std::filesystem::create_directories(documents, ec);

	const std::filesystem::path parent = DefaultNewProjectParent({}, documents);
	const std::filesystem::path composed = ComposeNewProjectRoot(parent, "AetherProject");

	CHECK(composed == (documents / "AetherCore Projects" / "AetherProject").lexically_normal());
	// The regression itself: the old seed was parent/name, which composed to name/name.
	CHECK(composed != (documents / "AetherCore Projects" / "AetherProject" / "AetherProject").lexically_normal());
}

TEST_CASE("DefaultNewProjectParent skips recents whose folder is gone")
{
	const std::filesystem::path root = MakeTempDir("missing");
	const std::filesystem::path live = root / "Live";
	const std::filesystem::path documents = root / "Documents";
	std::error_code ec;
	std::filesystem::create_directories(live / "Kept", ec);
	std::filesystem::create_directories(documents, ec);

	const std::vector<std::filesystem::path> recents{
	        root / "Vanished" / "Gone", // parent does not exist
	        live / "Kept",
	};
	CHECK(DefaultNewProjectParent(recents, documents) == live.lexically_normal());
}

TEST_CASE("DefaultNewProjectParent ignores empty roots and falls back to documents")
{
	const std::filesystem::path root = MakeTempDir("fallback");
	const std::filesystem::path documents = root / "Documents";
	std::error_code ec;
	std::filesystem::create_directories(documents, ec);

	const std::vector<std::filesystem::path> recents{std::filesystem::path{}, std::filesystem::path{}};
	CHECK(DefaultNewProjectParent(recents, documents) == (documents / "AetherCore Projects").lexically_normal());
}

TEST_CASE("DefaultNewProjectParent reports empty when it has nothing to go on")
{
	// The caller decides what to do with that (both fall back to the working directory);
	// this must not invent a path of its own.
	CHECK(DefaultNewProjectParent({}, {}).empty());
}
