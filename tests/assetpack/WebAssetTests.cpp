#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <chrono>
#include "assets/SpriteAtlasAsset.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/TileSetAsset.hpp"
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
	std::size_t reservedBytes = 0;
	CHECK_FALSE(archive->Read("caf\xc3\xa9.txt", [&](std::size_t size)
	{
		reservedBytes = size;
		return false;
	}).has_value());
	CHECK(reservedBytes == 4096);
	auto admitted = archive->Read("caf\xc3\xa9.txt", [](std::size_t size) { return size == 4096; });
	REQUIRE(admitted.has_value());
	CHECK(*admitted == *content);
	reservedBytes = 0;
	CHECK_FALSE(archive->Read("missing.txt", [&](std::size_t size)
	{
		reservedBytes = size;
		return true;
	}).has_value());
	CHECK(reservedBytes == 0);
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

TEST_CASE("Sprite and tile asset loaders reject future schemas")
{
	const auto directory = std::filesystem::temp_directory_path() / ("aether-asset-schema-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	const auto path = directory / "invalid.toml";
	{
		std::ofstream file(path);
		file << "[atlas]\nschema_version=999\n";
	}
	CHECK_FALSE(aether::SpriteAtlasAsset::Load(path).has_value());
	{
		std::ofstream file(path);
		file << "[animation]\nschema_version=999\n";
	}
	CHECK_FALSE(aether::SpriteAnimationAsset::Load(path).has_value());
	{
		std::ofstream file(path);
		file << "[tileset]\nschema_version=999\n";
	}
	CHECK_FALSE(aether::TileSetAsset::Load(path).has_value());
	std::filesystem::remove_all(directory);
}

TEST_CASE("Sprite and tile asset loaders bound allocation counts before allocating")
{
	const auto directory = std::filesystem::temp_directory_path() / ("aether-asset-count-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	const auto path = directory / "invalid.toml";
	for (const auto* number: {"65537", "-1", "nan", "1e30"})
	{
		{
			std::ofstream file(path);
			file << "[atlas]\nsprite_count=" << number << "\n";
		}
		CHECK_FALSE(aether::SpriteAtlasAsset::Load(path).has_value());
		{
			std::ofstream file(path);
			file << "[animation]\nframe_count=" << number << "\n";
		}
		CHECK_FALSE(aether::SpriteAnimationAsset::Load(path).has_value());
		{
			std::ofstream file(path);
			file << "[tileset]\ntile_count=" << number << "\n";
		}
		CHECK_FALSE(aether::TileSetAsset::Load(path).has_value());
	}
	std::filesystem::remove_all(directory);
}
