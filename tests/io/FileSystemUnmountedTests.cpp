// Reading through a mount point that has nothing mounted at it.
//
// This is a MISS, not a programmer error. Callers legitimately probe several prefixes and
// take the first that answers - FontRegistry tries project:// before engine:// so a game
// can override an engine typeface with its own.
//
// It used to assert. That made every such probe fatal in any application that mounts no
// project, which is precisely what the Launcher is: it is a project hub, so it has no
// project open by definition. The crash never appeared during development because a
// developer build mounts the sample project whose path CMake bakes in at configure time,
// so the one machine that could not reproduce it was the machine it was built on. It
// surfaced the first time a packaged Launcher was started somewhere else.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "io/FileSystem.hpp"

using namespace aether;

namespace
{
	// A prefix nothing will ever mount, so these cases cannot collide with whatever the
	// rest of the suite has set up.
	constexpr const char* kUnmounted = "no_such_mount_zzz://some/file.txt";

	// A mount of this test's own, so the fall-through case does not depend on how another
	// test left the shared engine:// mount. Initialises the file system only if nothing
	// else has yet, and never tears it down - the mounts are process-global.
	constexpr const char* kOwnMount = "ae_unmounted_test";

	void EnsureOwnMount()
	{
		if (!io::FileSystem::IsInitialized())
		{
			io::FileSystem::Initialize();
		}
		if (io::FileSystem::IsMounted(kOwnMount))
		{
			return;
		}
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ae_unmounted_mount_test";
		std::filesystem::create_directories(dir);
		std::ofstream(dir / "present.txt", std::ios::binary) << "PRESENT";
		io::FileSystem::Mount(kOwnMount, dir);
	}
} // namespace

TEST_CASE("An unmounted prefix reports a miss rather than aborting")
{
	EnsureOwnMount();

	SUBCASE("Exists is false")
	{
		CHECK_FALSE(io::FileSystem::Exists(kUnmounted));
	}

	SUBCASE("ReadFile fails, and says which mount point was missing")
	{
		const auto result = io::FileSystem::ReadFile(kUnmounted);
		REQUIRE_FALSE(result.has_value());
		// The message has to name the prefix: "file not found" would send someone looking
		// for a missing asset when the real answer is that nothing was mounted at all.
		CHECK(result.error().message.find("no_such_mount_zzz") != std::string::npos);
	}

	SUBCASE("ReadFileText fails the same way")
	{
		CHECK_FALSE(io::FileSystem::ReadFileText(kUnmounted).has_value());
	}

	SUBCASE("OpenStream fails")
	{
		CHECK_FALSE(io::FileSystem::OpenStream(kUnmounted).has_value());
	}

	SUBCASE("WriteFile fails instead of inventing a destination")
	{
		const std::string text = "x";
		CHECK_FALSE(io::FileSystem::WriteFileText(kUnmounted, text).has_value());
	}

	SUBCASE("Glob contributes no matches, like an empty directory")
	{
		const auto result = io::FileSystem::Glob("no_such_mount_zzz://*.txt");
		REQUIRE(result.has_value());
		CHECK(result->empty());
	}
}

// The behaviour FontRegistry depends on: probe an optional prefix, fall through to one
// that is mounted, and take the first answer.
TEST_CASE("A probe over several prefixes falls through an unmounted one")
{
	EnsureOwnMount();

	const char* const candidates[] = {"no_such_mount_zzz://present.txt", "ae_unmounted_test://present.txt"};

	int tried = 0;
	bool found = false;
	for (const char* candidate: candidates)
	{
		++tried;
		if (io::FileSystem::ReadFile(candidate).has_value())
		{
			found = true;
			break;
		}
	}

	CHECK(tried == 2); // the first candidate was attempted and simply did not answer
	CHECK(found);
}
