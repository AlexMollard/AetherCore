// The editor's delete goes through MoveToTrash so a mistaken delete is recoverable. A
// permanent remove_all on a folder took the whole subtree with no way back.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

// The .trashinfo body decides whether a Linux desktop can restore the file. It is built on
// every platform so these rules can be checked without a Linux desktop - a malformed Path=
// leaves a file sitting in the trash that nothing can put back.
TEST_CASE("BuildTrashInfo writes a restorable freedesktop entry")
{
	const std::string info = aether::io::file_util::BuildTrashInfo("/home/dev/Assets/hero.png", "2026-09-02T11:00:00");

	CHECK(info.starts_with("[Trash Info]"));
	CHECK(info.find("Path=/home/dev/Assets/hero.png") != std::string::npos);
	CHECK(info.find("DeletionDate=2026-09-02T11:00:00") != std::string::npos);
}

TEST_CASE("BuildTrashInfo percent-encodes what would break the entry, but not separators")
{
	const std::string info = aether::io::file_util::BuildTrashInfo("/home/dev/My Assets/a#b.png", "2026-09-02T11:00:00");

	// Spaces and '#' must be encoded - '#' starts a comment in the desktop file format, so an
	// unencoded one truncates the path and the entry silently points somewhere else.
	CHECK(info.find("My%20Assets") != std::string::npos);
	CHECK(info.find("a%23b.png") != std::string::npos);
	// Separators must survive, or the path is meaningless.
	CHECK(info.find("Path=/home/dev/") != std::string::npos);
}

TEST_CASE("BuildTrashInfo leaves unreserved characters alone")
{
	const std::string info = aether::io::file_util::BuildTrashInfo("/a/b-c_d.e~f/g.png", "2026-01-01T00:00:00");

	CHECK(info.find("Path=/a/b-c_d.e~f/g.png") != std::string::npos);
}
