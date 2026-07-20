#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "AssetPipeline.hpp"
#include "PakWriter.hpp"
#include "assets/TileMapAsset.hpp"
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

TEST_CASE("PackProject ships tilemap .tiles assets with their current on-disk bytes")
{
	// Regression guard for the tilemap-publish bug: the pack must include the .tiles
	// asset and ship exactly what is on disk (which the save/publish flush writes).
	const std::filesystem::path projectRoot = MakeTempDir("proj_tiles");
	WriteFile(projectRoot / "ProjectSettings.toml", "[project]\nversion = 1\nname = \"Test\"\n");

	aether::TileMapAsset map;
	map.layers.emplace_back();
	map.SetCell(0, {3, 4}, aether::tilecell::Make(0));
	const std::filesystem::path tilesPath = projectRoot / "assets" / "tiles" / "level.atlm";
	std::filesystem::create_directories(tilesPath.parent_path());
	REQUIRE(map.Save(tilesPath).has_value());

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_tiles.pak";
	std::filesystem::remove(pak);
	const assetpipeline::PackResult result = assetpipeline::PackProject(projectRoot, pak, {.projectLayout = true});
	REQUIRE(result.ok);

	io::PakBackend backend(pak);
	REQUIRE(backend.Exists("assets/tiles/level.atlm"));

	const auto packed = backend.Read("assets/tiles/level.atlm");
	REQUIRE(packed.has_value());
	std::ifstream in(tilesPath, std::ios::binary);
	const std::vector<char> onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	REQUIRE(packed->size() == onDisk.size());
	CHECK(std::equal(onDisk.begin(), onDisk.end(), reinterpret_cast<const char*>(packed->data())));
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

TEST_CASE("PakWriter::AddDirectoryAs packs a directory's files under a virtual prefix")
{
	const std::filesystem::path shaderDir = MakeTempDir("adddiras_shaders");
	WriteFile(shaderDir / "testcustom.spv", "fake-spirv-bytes");
	WriteFile(shaderDir / "nested" / "other.spv", "fake-spirv-bytes-2");
	WriteFile(shaderDir / "testcustom.slangc.log", "slangc build log, must not ship");

	assetpipeline::PakWriter writer(/*compressionLevel=*/0);
	writer.AddDirectoryAs(shaderDir, "shaders");
	CHECK(writer.FileCount() == 2);

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_adddiras.pak";
	std::filesystem::remove(pak);
	REQUIRE(writer.Write(pak));

	io::PakBackend backend(pak);
	CHECK(backend.Exists("shaders/testcustom.spv"));
	CHECK(backend.Exists("shaders/nested/other.spv"));
	CHECK_FALSE(backend.Exists("testcustom.spv"));
	CHECK_FALSE(backend.Exists("shaders/testcustom.slangc.log"));

	const auto bytes = backend.Read("shaders/testcustom.spv");
	REQUIRE(bytes.has_value());
	const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
	CHECK(text == "fake-spirv-bytes");
}

TEST_CASE("PackProject with shaderSpirvDir packs compiled shaders into the same pak's shaders/ prefix, excluding .slang sources")
{
	const std::filesystem::path projectRoot = MakeTempDir("shaderpack_project");
	WriteFile(projectRoot / "ProjectSettings.toml", "[project]\nversion = 1\nname = \"Test\"\n");
	WriteFile(projectRoot / "assets" / "shaders" / "testcustom.slang", "// slang source, should not be packed");
	WriteFile(projectRoot / "assets" / "readme.txt", "regular project asset");

	const std::filesystem::path shaderSpirvDir = MakeTempDir("shaderpack_spirv");
	WriteFile(shaderSpirvDir / "testcustom.spv", "compiled-spirv-bytes");

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_shaderpack.pak";
	std::filesystem::remove(pak);

	const assetpipeline::PackResult result = assetpipeline::PackProject(projectRoot, pak, {.projectLayout = true, .shaderSpirvDir = shaderSpirvDir});
	REQUIRE(result.ok);

	io::PakBackend backend(pak);
	CHECK(backend.Exists("shaders/testcustom.spv"));
	CHECK(backend.Exists("assets/readme.txt"));
	CHECK_FALSE(backend.Exists("assets/shaders/testcustom.slang"));

	const auto bytes = backend.Read("shaders/testcustom.spv");
	REQUIRE(bytes.has_value());
	const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
	CHECK(text == "compiled-spirv-bytes");
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
