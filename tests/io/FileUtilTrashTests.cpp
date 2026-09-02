// The editor's delete goes through MoveToTrash so a mistaken delete is recoverable. A
// permanent remove_all on a folder took the whole subtree with no way back.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "io/FileUtil.hpp"

// Gated: a passing run leaves an entry in the developer's recycle bin, and the suite runs
// many times a day. Set AETHER_TEST_TRASH=1 to exercise it. Verified by hand on Windows -
// the bin went from 351 to 352 items and the file was in it by name.
TEST_CASE("MoveToTrash removes the file from its original location")
{
	if (std::getenv("AETHER_TEST_TRASH") == nullptr)
	{
		return;
	}
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_trash_probe";
	std::filesystem::create_directories(dir);
	const std::filesystem::path file = dir / "delete_me.txt";
	{
		std::ofstream out(file);
		out << "probe";
	}
	REQUIRE(std::filesystem::exists(file));

	const bool trashed = aether::io::file_util::MoveToTrash(file);

#if defined(_WIN32)
	// Windows has a shell trash, so this must succeed and the file must be gone from here.
	CHECK(trashed);
	CHECK_FALSE(std::filesystem::exists(file));
#else
	// No implementation yet: it must report failure rather than deleting anything, so the
	// caller knows to fall back rather than believing the file was safely trashed.
	CHECK_FALSE(trashed);
	CHECK(std::filesystem::exists(file));
	std::filesystem::remove(file);
#endif
}

TEST_CASE("MoveToTrash reports failure for a path that does not exist")
{
	const std::filesystem::path missing = std::filesystem::temp_directory_path() / "aether_trash_probe_missing_zzz";
	std::filesystem::remove(missing);

	CHECK_FALSE(aether::io::file_util::MoveToTrash(missing));
}
