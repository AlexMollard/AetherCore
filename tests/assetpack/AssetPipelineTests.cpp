#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "AssetPipeline.hpp"
#include "io/PakBackend.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeTempDir(const std::string& tag)
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("aepak_test_" + tag);
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
}

TEST_CASE("PackDirectory round-trips plain files through PakBackend")
{
	const std::filesystem::path src = MakeTempDir("roundtrip_src");
	WriteFile(src / "hello.txt", "hello world");
	WriteFile(src / "nested" / "data.bytes", std::string(4096, 'x'));

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_roundtrip.pak";
	std::filesystem::remove(pak);

	const assetpipeline::PackResult result = assetpipeline::PackDirectory(src, pak, {});
	REQUIRE(result.ok);
	CHECK(std::filesystem::exists(pak));

	io::PakBackend backend(pak);
	CHECK(backend.Exists("hello.txt"));
	CHECK(backend.Exists("nested/data.bytes"));

	const auto bytes = backend.Read("hello.txt");
	REQUIRE(bytes.has_value());
	const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
	CHECK(text == "hello world");
}

TEST_CASE("PackProject fails cleanly when the descriptor is missing")
{
	const std::filesystem::path src = MakeTempDir("noproj");
	WriteFile(src / "a.txt", "a");

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_noproj.pak";
	const assetpipeline::PackResult result = assetpipeline::PackProject(src, pak, {.projectLayout = true});
	CHECK_FALSE(result.ok);
	CHECK(result.message.find("descriptor is missing") != std::string::npos);
}
