#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "io/FileUtil.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
	using namespace aether;

	struct AtomicWriteDirectory
	{
		std::filesystem::path root;

		AtomicWriteDirectory()
		{
			static std::atomic<unsigned> s_nextId{0};
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			root = std::filesystem::temp_directory_path() / ("aether_atomic_" + std::to_string(stamp) + "_" + std::to_string(s_nextId.fetch_add(1)));
			REQUIRE(std::filesystem::create_directory(root));
		}

		~AtomicWriteDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	};

	std::vector<std::filesystem::path> Inventory(const std::filesystem::path& directory)
	{
		std::vector<std::filesystem::path> entries;
		for (const auto& entry : std::filesystem::directory_iterator(directory))
		{
			entries.push_back(entry.path().filename());
		}
		std::ranges::sort(entries);
		return entries;
	}
} // namespace

TEST_CASE("Atomic text writes create and replace complete destination contents")
{
	const AtomicWriteDirectory directory;
	const auto destination = directory.root / "scene.toml";
	REQUIRE(io::file_util::WriteTextAtomic(destination, "original").has_value());
	const auto original = io::file_util::ReadText(destination);
	REQUIRE(original.has_value());
	CHECK(*original == "original");
	const auto before = Inventory(directory.root);

	std::string replacement("replacement\0bytes", 17);
	SUBCASE("Replacing with an empty file")
	{
		replacement.clear();
	}
	SUBCASE("Replacing with bytes including an embedded null")
	{
	}

	REQUIRE(io::file_util::WriteTextAtomic(destination, replacement).has_value());
	const auto contents = io::file_util::ReadText(destination);
	REQUIRE(contents.has_value());
	CHECK(*contents == replacement);
	CHECK(Inventory(directory.root) == before);
}

TEST_CASE("Atomic text write refuses a parent occupied by a file without changing it")
{
	const AtomicWriteDirectory directory;
	const auto occupied = directory.root / "occupied";
	REQUIRE(io::file_util::WriteText(occupied, "keep these bytes").has_value());
	const auto savedTime = std::filesystem::last_write_time(occupied);
	const auto before = Inventory(directory.root);

	CHECK_FALSE(io::file_util::WriteTextAtomic(occupied / "scene.toml", "replacement").has_value());
	const auto contents = io::file_util::ReadText(occupied);
	REQUIRE(contents.has_value());
	CHECK(*contents == "keep these bytes");
	CHECK(std::filesystem::last_write_time(occupied) == savedTime);
	CHECK(Inventory(directory.root) == before);
}

#ifdef _WIN32
TEST_CASE("Atomic temporary open failure preserves an existing maximum component destination")
{
	AtomicWriteDirectory directory;
	wchar_t volumePath[MAX_PATH]{};
	REQUIRE(GetVolumePathNameW(directory.root.c_str(), volumePath, MAX_PATH) != FALSE);
	DWORD maximumComponentLength = 0;
	REQUIRE(GetVolumeInformationW(volumePath, nullptr, 0, nullptr, &maximumComponentLength, nullptr, nullptr, 0) != FALSE);
	REQUIRE(maximumComponentLength > 0);

	// Bypass Win32's total-path limit so only the volume's component limit is tested.
	// The existing destination fits exactly; appending the unique temp suffix cannot fit.
	const auto absolute = std::filesystem::absolute(directory.root).wstring();
	directory.root = absolute.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + absolute.substr(2) : L"\\\\?\\" + absolute;
	const auto destination = directory.root / std::wstring(maximumComponentLength, L'x');
	REQUIRE(io::file_util::WriteText(destination, "previous saved scene").has_value());
	const auto savedTime = std::filesystem::last_write_time(destination);
	const auto before = Inventory(directory.root);

	const auto result = io::file_util::WriteTextAtomic(destination, "new scene");
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().message.find("create temporary file for") != std::string::npos);
	const auto contents = io::file_util::ReadText(destination);
	REQUIRE(contents.has_value());
	CHECK(*contents == "previous saved scene");
	CHECK(std::filesystem::last_write_time(destination) == savedTime);
	CHECK(Inventory(directory.root) == before);
}
#endif
