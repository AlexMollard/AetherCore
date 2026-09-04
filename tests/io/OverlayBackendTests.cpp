#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "io/DirectoryBackend.hpp"
#include "io/OverlayBackend.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeTempDir(const std::string& tag)
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("ae_overlay_test_" + tag);
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		return dir;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& contents)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		out << contents;
	}

	std::string ToString(const std::vector<std::byte>& bytes)
	{
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}
}

TEST_CASE("reads from the only layer that has the file")
{
	const std::filesystem::path dirA = MakeTempDir("single_layer");
	WriteFile(dirA / "a.txt", "hello from a");

	auto backendA = std::make_shared<io::DirectoryBackend>(dirA);
	io::OverlayBackend overlay(std::vector<io::OverlayBackend::Layer>{
	        {.backend = backendA, .prefix = ""},
	});

	CHECK(overlay.Exists("a.txt"));
	CHECK_FALSE(overlay.Exists("nope.txt"));

	const auto bytes = overlay.Read("a.txt");
	REQUIRE(bytes.has_value());
	CHECK(ToString(*bytes) == "hello from a");

	const auto missing = overlay.Read("nope.txt");
	CHECK_FALSE(missing.has_value());
}

TEST_CASE("higher-priority layer overrides a same-named file")
{
	const std::filesystem::path dirA = MakeTempDir("priority_a");
	const std::filesystem::path dirB = MakeTempDir("priority_b");
	WriteFile(dirA / "shared.txt", "from A");
	WriteFile(dirB / "shared.txt", "from B");

	auto backendA = std::make_shared<io::DirectoryBackend>(dirA);
	auto backendB = std::make_shared<io::DirectoryBackend>(dirB);

	io::OverlayBackend overlay(std::vector<io::OverlayBackend::Layer>{
	        {.backend = backendA, .prefix = ""},
	        {.backend = backendB, .prefix = ""},
	});

	const auto bytes = overlay.Read("shared.txt");
	REQUIRE(bytes.has_value());
	CHECK(ToString(*bytes) == "from A");
}

TEST_CASE("Glob unions both layers and de-dups overlapping names")
{
	const std::filesystem::path dirA = MakeTempDir("glob_a");
	const std::filesystem::path dirB = MakeTempDir("glob_b");
	WriteFile(dirA / "a_only.txt", "a only");
	WriteFile(dirA / "shared.txt", "from A");
	WriteFile(dirB / "b_only.txt", "b only");
	WriteFile(dirB / "shared.txt", "from B");

	auto backendA = std::make_shared<io::DirectoryBackend>(dirA);
	auto backendB = std::make_shared<io::DirectoryBackend>(dirB);

	io::OverlayBackend overlay(std::vector<io::OverlayBackend::Layer>{
	        {.backend = backendA, .prefix = ""},
	        {.backend = backendB, .prefix = ""},
	});

	const auto matches = overlay.Glob("*.txt", io::FileGlobOptions{});
	REQUIRE(matches.has_value());

	CHECK(matches->size() == 3);

	int sharedCount = 0;
	for (const auto& path: *matches)
	{
		if (path == "shared.txt")
		{
			++sharedCount;
		}
	}
	CHECK(sharedCount == 1);

	const auto bytes = overlay.Read("shared.txt");
	REQUIRE(bytes.has_value());
	CHECK(ToString(*bytes) == "from A");
}

TEST_CASE("a prefixed layer resolves rel under the prefix")
{
	const std::filesystem::path dirC = MakeTempDir("prefixed");
	WriteFile(dirC / "sub" / "x.txt", "nested content");

	auto backendC = std::make_shared<io::DirectoryBackend>(dirC);
	io::OverlayBackend overlay(std::vector<io::OverlayBackend::Layer>{
	        {.backend = backendC, .prefix = "sub/"},
	});

	CHECK(overlay.Exists("x.txt"));

	const auto bytes = overlay.Read("x.txt");
	REQUIRE(bytes.has_value());
	CHECK(ToString(*bytes) == "nested content");
}

TEST_CASE("Glob strips a layer prefix case-insensitively")
{
	// whose on-disk directory casing ("Shaders/") differs from the layer's
	const std::filesystem::path dirD = MakeTempDir("case_insensitive_prefix");
	WriteFile(dirD / "Shaders" / "foo.spv", "spv bytes");

	auto backendD = std::make_shared<io::DirectoryBackend>(dirD);
	io::OverlayBackend overlay(std::vector<io::OverlayBackend::Layer>{
	        {.backend = backendD, .prefix = "shaders/"},
	});

	const auto matches = overlay.Glob("*.spv", io::FileGlobOptions{});
	REQUIRE(matches.has_value());
	REQUIRE(matches->size() == 1);
	CHECK((*matches)[0] == "foo.spv");

	CHECK(overlay.Exists("foo.spv"));
	const auto bytes = overlay.Read("foo.spv");
	REQUIRE(bytes.has_value());
	CHECK(ToString(*bytes) == "spv bytes");
}

TEST_CASE("DirectoryBackend rejects paths that escape the mount root")
{
	const std::filesystem::path dir = MakeTempDir("traversal");
	WriteFile(dir / "inside.txt", "inside");
	const std::filesystem::path outsideFile = std::filesystem::temp_directory_path() / "ae_overlay_test_traversal_outside.txt";
	WriteFile(outsideFile, "outside");

	const io::DirectoryBackend backend(dir);

	// Reads, existence checks and a '..' that stays inside the mount keep working.
	REQUIRE(backend.Read("inside.txt").has_value());
	CHECK(ToString(*backend.Read("inside.txt")) == "inside");
	CHECK(backend.Exists("sub/../inside.txt"));

	// Every escape spelling must fail for read, stream and write alike.
	const std::vector<std::string> escapes = {
	        "../ae_overlay_test_traversal_outside.txt",
	        "a/../../ae_overlay_test_traversal_outside.txt",
	        ".././../ae_overlay_test_traversal_outside.txt",
	        outsideFile.generic_string(),
	};
	for (const std::string& escape: escapes)
	{
		CAPTURE(escape);
		CHECK_FALSE(backend.Exists(escape));
		CHECK_FALSE(backend.Read(escape).has_value());
		CHECK_FALSE(backend.OpenStream(escape).has_value());
		CHECK_FALSE(backend.Write(escape, std::span<const std::byte>{}).has_value());
	}

	// The rejected writes must not have touched the file they targeted.
	std::ifstream in(outsideFile, std::ios::binary | std::ios::ate);
	REQUIRE(in);
	std::string contents(static_cast<std::size_t>(in.tellg()), '\0');
	in.seekg(0, std::ios::beg);
	in.read(contents.data(), static_cast<std::streamsize>(contents.size()));
	CHECK(contents == "outside");
}
