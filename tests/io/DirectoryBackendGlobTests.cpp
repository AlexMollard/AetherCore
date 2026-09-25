#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "io/DirectoryBackend.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeGlobTree()
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path() / "ae_directory_glob_test";
		std::filesystem::remove_all(root);
		for (const char* rel: {"assets/levels/hub/beach/scenery.gltf", "assets/levels/hub/beach/sky.gltf", "assets/levels/hub/pier/scenery.gltf", "assets/models/crate.gltf", "top.gltf"})
		{
			const std::filesystem::path p = root / rel;
			std::filesystem::create_directories(p.parent_path());
			std::ofstream(p) << "x";
		}
		return root;
	}

	std::vector<std::string> GlobOf(const io::DirectoryBackend& backend, std::string_view pattern, io::FileGlobOptions options = {})
	{
		auto result = backend.Glob(pattern, options);
		REQUIRE(result.has_value());
		return *result;
	}
}

// DirectoryBackend::Glob walks only the pattern's wildcard-free directory prefix; these pin
// that the narrowed walk returns exactly what a whole-mount match would.
TEST_CASE("DirectoryBackend Glob matches through the literal directory prefix only")
{
	const io::DirectoryBackend backend(MakeGlobTree());

	CHECK(GlobOf(backend, "assets/levels/hub/*/scenery.gltf") == std::vector<std::string>{"assets/levels/hub/beach/scenery.gltf", "assets/levels/hub/pier/scenery.gltf"});
	CHECK(GlobOf(backend, "assets/levels/**/*.gltf").size() == 3);
	CHECK(GlobOf(backend, "**/scenery.gltf").size() == 2);
	CHECK(GlobOf(backend, "*.gltf") == std::vector<std::string>{"top.gltf"});
	CHECK(GlobOf(backend, "assets/nowhere/*.gltf").empty());
}

TEST_CASE("DirectoryBackend Glob of a literal path returns it only when it is a file")
{
	const io::DirectoryBackend backend(MakeGlobTree());

	CHECK(GlobOf(backend, "assets/levels/hub/beach/sky.gltf") == std::vector<std::string>{"assets/levels/hub/beach/sky.gltf"});
	CHECK(GlobOf(backend, "assets/levels/hub/bossarea/scenery.gltf").empty());
	CHECK(GlobOf(backend, "assets/levels/hub/beach").empty());
	CHECK(GlobOf(backend, "assets/levels/hub/beach", io::FileGlobOptions{.includeDirectories = true}) == std::vector<std::string>{"assets/levels/hub/beach"});
}

TEST_CASE("DirectoryBackend ContentStamp changes when the file changes and holds on untouched files")
{
	const std::filesystem::path root = MakeGlobTree();
	const io::DirectoryBackend backend(root);
	const auto file = std::filesystem::path("assets") / "models" / "crate.gltf";

	const std::uint64_t stampA = backend.ContentStamp(file.generic_string());
	REQUIRE(stampA != 0);
	CHECK(backend.ContentStamp(file.generic_string()) == stampA);
	CHECK(backend.ContentStamp("assets/models/missing.gltf") == 0);

	// Touch mtime AND size so the stamp moves even on coarse filesystem timestamps.
	std::filesystem::last_write_time(root / file, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
	{ std::ofstream out(root / file, std::ios::app); out << "changed"; }

	CHECK(backend.ContentStamp(file.generic_string()) != stampA);
}

TEST_CASE("DirectoryBackend Glob never reaches outside the mount root")
{
	const std::filesystem::path root = MakeGlobTree();
	const io::DirectoryBackend backend(root / "assets");

	CHECK(GlobOf(backend, "../top.gltf").empty());
	CHECK(GlobOf(backend, "../*.gltf").empty());
	CHECK(GlobOf(backend, "levels/../../*.gltf").empty());
}
