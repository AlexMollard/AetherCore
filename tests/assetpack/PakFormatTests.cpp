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
