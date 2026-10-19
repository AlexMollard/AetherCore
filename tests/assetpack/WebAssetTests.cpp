#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include "AssetPipeline.hpp"
#include "io/MemoryPak.hpp"
#include "WebAssetFormat.hpp"

TEST_CASE("Web package validates target UTF8 compression versions and hashes")
{
	const auto directory = std::filesystem::temp_directory_path() / "aether-web-pak-test";
	std::filesystem::create_directories(directory / "source");
	const auto source = directory / "source";
	const auto path = directory / "game.pak";
	std::ofstream(source / std::filesystem::path(std::u8string(u8"caf\u00e9.txt")), std::ios::binary) << std::string(4096, 'x');
	const auto result = aether::assetpipeline::PackDirectory(source, path, {.compressionLevel = 3, .target = "webgpu-2d-v1"});
	REQUIRE(result.ok);
	std::ifstream stream(path, std::ios::binary);
	std::vector<char> raw(std::istreambuf_iterator<char>(stream), {});
	stream.close();
	std::vector<std::byte> bytes(raw.size());
	std::memcpy(bytes.data(), raw.data(), raw.size());
	auto archive = aether::io::MemoryPak::Open(bytes, "webgpu-2d-v1");
	REQUIRE(archive.has_value());
	auto content = archive->Read("caf\xc3\xa9.txt");
	REQUIRE(content.has_value());
	CHECK(content->size() == 4096);
	CHECK_FALSE(archive->Read("missing.txt").has_value());
	CHECK_FALSE(aether::io::MemoryPak::Open(bytes, "desktop").has_value());
	auto corrupt = bytes;
	corrupt[4] = std::byte{99};
	CHECK_FALSE(aether::io::MemoryPak::Open(corrupt, "webgpu-2d-v1").has_value());
	corrupt = bytes;
	corrupt.resize(20);
	CHECK_FALSE(aether::io::MemoryPak::Open(corrupt, "webgpu-2d-v1").has_value());
	corrupt = bytes;
	corrupt[64] ^= std::byte{1};
	CHECK_FALSE(aether::io::MemoryPak::Open(corrupt, "webgpu-2d-v1").has_value());
	std::filesystem::remove_all(directory);
}

TEST_CASE("Web texture validates layout format version and bounded dimensions")
{
	WebTextureHeader header;
	header.width = 2;
	header.height = 2;
	header.pixelBytes = 16;
	std::vector<std::byte> bytes(sizeof(header) + 16);
	std::memcpy(bytes.data(), &header, sizeof(header));
	WebTextureHeader parsed;
	CHECK(ReadWebTextureHeader(bytes, parsed));
	header.format = 99;
	std::memcpy(bytes.data(), &header, sizeof(header));
	CHECK_FALSE(ReadWebTextureHeader(bytes, parsed));
	header.format = 1;
	header.width = 5000;
	std::memcpy(bytes.data(), &header, sizeof(header));
	CHECK_FALSE(ReadWebTextureHeader(bytes, parsed));
}
