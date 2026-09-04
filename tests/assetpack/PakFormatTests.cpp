#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "AssetPipeline.hpp"
#include "io/PakBackend.hpp"
#include "utils/AetherExceptions.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeValidPak(const std::string& tag)
	{
		const std::filesystem::path src = std::filesystem::temp_directory_path() / ("aepak_fmt_src_" + tag);
		std::filesystem::remove_all(src);
		std::filesystem::create_directories(src);
		std::ofstream(src / "a.txt", std::ios::binary) << "content";

		const std::filesystem::path pak = std::filesystem::temp_directory_path() / ("aepak_fmt_" + tag + ".pak");
		std::filesystem::remove(pak);
		const auto result = assetpipeline::PackDirectory(src, pak, {});
		REQUIRE(result.ok);
		return pak;
	}

	std::vector<char> ReadAll(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary | std::ios::ate);
		std::vector<char> buf(static_cast<std::size_t>(in.tellg()));
		in.seekg(0);
		in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
		return buf;
	}

	void WriteAll(const std::filesystem::path& p, const std::vector<char>& buf)
	{
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
	}
}

TEST_CASE("A valid pak opens")
{
	const auto pak = MakeValidPak("valid");
	CHECK_NOTHROW(io::PakBackend{pak});
}

TEST_CASE("A corrupted index byte is rejected")
{
	const auto pak = MakeValidPak("corrupt");
	auto bytes = ReadAll(pak);
	bytes.at(70) ^= 0xFF;
	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A truncated pak is rejected")
{
	const auto pak = MakeValidPak("trunc");
	auto bytes = ReadAll(pak);
	bytes.resize(bytes.size() / 2);
	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A pak with overflowing index sizes is rejected")
{
	const auto pak = MakeValidPak("overflow");
	auto bytes = ReadAll(pak);
	for (int i = 0; i < 8; ++i)
	{
		bytes.at(24 + i) = static_cast<char>(0xFF);
	}
	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A pak with an implausible entry count is rejected")
{
	const auto pak = MakeValidPak("bignum");
	auto bytes = ReadAll(pak);
	const std::uint32_t tooMany = 16u * 1024u * 1024u;
	std::memcpy(bytes.data() + 8, &tooMany, sizeof(tooMany));
	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A pak with an out-of-range entry but valid index hash is rejected")
{
	const auto pak = MakeValidPak("badentry");
	auto bytes = ReadAll(pak);

	std::uint32_t numEntries = 0;
	std::uint64_t pathDataOffset = 0, pathDataSize = 0;
	std::memcpy(&numEntries, bytes.data() + 8, sizeof(numEntries));
	std::memcpy(&pathDataOffset, bytes.data() + 16, sizeof(pathDataOffset));
	std::memcpy(&pathDataSize, bytes.data() + 24, sizeof(pathDataSize));
	REQUIRE(numEntries >= 1);

	// this must be caught by the per-entry range check, not the header-level checks).
	const std::uint64_t hugeDataSize = 0xFFFFFFFFull;
	std::memcpy(bytes.data() + 64 + 24, &hugeDataSize, sizeof(hugeDataSize));

	const std::size_t entryTableSize = static_cast<std::size_t>(numEntries) * 40;
	XXH3_state_t* st = XXH3_createState();
	XXH3_64bits_reset(st);
	XXH3_64bits_update(st, bytes.data() + 64, entryTableSize);
	XXH3_64bits_update(st, bytes.data() + static_cast<std::size_t>(pathDataOffset), static_cast<std::size_t>(pathDataSize));
	const std::uint64_t newHash = XXH3_64bits_digest(st);
	XXH3_freeState(st);
	std::memcpy(bytes.data() + 48, &newHash, sizeof(newHash));

	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A pak entry whose path range leaves the path data is rejected")
{
	const auto pak = MakeValidPak("badpath");
	auto bytes = ReadAll(pak);

	std::uint32_t numEntries = 0;
	std::uint64_t pathDataOffset = 0, pathDataSize = 0;
	std::memcpy(&numEntries, bytes.data() + 8, sizeof(numEntries));
	std::memcpy(&pathDataOffset, bytes.data() + 16, sizeof(pathDataOffset));
	std::memcpy(&pathDataSize, bytes.data() + 24, sizeof(pathDataSize));
	REQUIRE(numEntries >= 1);

	// pathOffset far past the end of the path data with the index hash
	// recomputed, so the per-entry range check is what must catch it (the same
	// recipe as the out-of-range-dataSize case above, aimed at the path fields).
	const std::uint32_t bogusOffset = 0xFFFFFFF0u;
	std::memcpy(bytes.data() + 64, &bogusOffset, sizeof(bogusOffset));
	const std::uint32_t bogusLen = 0x20u;
	std::memcpy(bytes.data() + 64 + 4, &bogusLen, sizeof(bogusLen));

	const std::size_t entryTableSize = static_cast<std::size_t>(numEntries) * 40;
	XXH3_state_t* st = XXH3_createState();
	XXH3_64bits_reset(st);
	XXH3_64bits_update(st, bytes.data() + 64, entryTableSize);
	XXH3_64bits_update(st, bytes.data() + static_cast<std::size_t>(pathDataOffset), static_cast<std::size_t>(pathDataSize));
	const std::uint64_t newHash = XXH3_64bits_digest(st);
	XXH3_freeState(st);
	std::memcpy(bytes.data() + 48, &newHash, sizeof(newHash));

	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A zstd frame declaring an absurd decompressed size is rejected, not allocated")
{
	const auto pak = MakeValidPak("hugedecl");
	auto bytes = ReadAll(pak);

	std::uint32_t numEntries = 0;
	std::uint64_t pathDataOffset = 0, pathDataSize = 0, assetDataOffset = 0;
	std::memcpy(&numEntries, bytes.data() + 8, sizeof(numEntries));
	std::memcpy(&pathDataOffset, bytes.data() + 16, sizeof(pathDataOffset));
	std::memcpy(&pathDataSize, bytes.data() + 24, sizeof(pathDataSize));
	std::memcpy(&assetDataOffset, bytes.data() + 32, sizeof(assetDataOffset));
	REQUIRE(numEntries >= 1);

	// Retarget the first non-manifest entry at a crafted zstd frame whose
	// header declares a 1 TiB content size; nothing on disk grows, so only the
	// read-side bound on the declared size can keep the allocation from happening.
	std::size_t targetEntry = SIZE_MAX;
	std::string targetPath;
	for (std::uint32_t i = 0; i < numEntries; ++i)
	{
		const char* entry = bytes.data() + 64 + static_cast<std::size_t>(i) * 40;
		std::uint32_t pathOffset = 0, pathLen = 0;
		std::memcpy(&pathOffset, entry, sizeof(pathOffset));
		std::memcpy(&pathLen, entry + 4, sizeof(pathLen));
		std::string path(bytes.data() + static_cast<std::size_t>(pathDataOffset) + pathOffset, pathLen);
		if (path != "__aetherpak/manifest.txt")
		{
			targetEntry = i;
			targetPath = std::move(path);
			break;
		}
	}
	REQUIRE(targetEntry != SIZE_MAX);

	// zstd frame header: magic, descriptor (single segment, 8-byte frame content
	// size), then the declared content size - 2^40 bytes.
	const std::uint64_t declared = 1ull << 40;
	const unsigned char frameHeader[] = {0x28, 0xB5, 0x2D, 0xFD, 0xE0};
	const std::size_t frameOffset = bytes.size();
	bytes.insert(bytes.end(), reinterpret_cast<const char*>(frameHeader), reinterpret_cast<const char*>(frameHeader) + sizeof(frameHeader));
	for (int b = 0; b < 8; ++b)
	{
		bytes.push_back(static_cast<char>((declared >> (8 * b)) & 0xFF));
	}

	char* entry = bytes.data() + 64 + targetEntry * 40;
	const std::uint64_t newDataOffset = static_cast<std::uint64_t>(frameOffset) - assetDataOffset;
	const std::uint32_t newDataSize = static_cast<std::uint32_t>(bytes.size() - frameOffset);
	const std::uint32_t newFlags = PAK_FLAG_ZSTD;
	std::memcpy(entry + 8, &newFlags, sizeof(newFlags));
	std::memcpy(entry + 16, &newDataOffset, sizeof(newDataOffset));
	std::memcpy(entry + 24, &newDataSize, sizeof(newDataSize));

	// Grow assetDataSize so the relocated entry stays in range, then re-hash.
	const std::uint64_t newAssetDataSize = static_cast<std::uint64_t>(bytes.size()) - assetDataOffset;
	std::memcpy(bytes.data() + 40, &newAssetDataSize, sizeof(newAssetDataSize));

	const std::size_t entryTableSize = static_cast<std::size_t>(numEntries) * 40;
	XXH3_state_t* st = XXH3_createState();
	XXH3_64bits_reset(st);
	XXH3_64bits_update(st, bytes.data() + 64, entryTableSize);
	XXH3_64bits_update(st, bytes.data() + static_cast<std::size_t>(pathDataOffset), static_cast<std::size_t>(pathDataSize));
	const std::uint64_t newHash = XXH3_64bits_digest(st);
	XXH3_freeState(st);
	std::memcpy(bytes.data() + 48, &newHash, sizeof(newHash));

	WriteAll(pak, bytes);

	io::PakBackend backend(pak);
	const auto result = backend.Read(targetPath);
	CHECK_FALSE(result.has_value());
}
