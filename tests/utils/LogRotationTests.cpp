#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "utils/LogRotation.hpp"

using aether::RotateLogIfLarge;

namespace
{
	struct TempDir
	{
		std::filesystem::path path;

		TempDir()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			path = std::filesystem::temp_directory_path() / ("aether_log_rotation_" + std::to_string(stamp));
			std::filesystem::create_directories(path);
		}

		~TempDir()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	void WriteFile(const std::filesystem::path& file, const std::string& contents)
	{
		std::ofstream{file} << contents;
	}

	std::string ReadFile(const std::filesystem::path& file)
	{
		std::ifstream input{file};
		return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
	}
} // namespace

TEST_CASE("RotateLogIfLarge leaves a log that is under the limit alone")
{
	const TempDir temp;
	const auto log = temp.path / "Editor.log";
	WriteFile(log, "small");

	CHECK_FALSE(RotateLogIfLarge(log, 1024));
	CHECK(std::filesystem::exists(log));
	CHECK(ReadFile(log) == "small");
	CHECK_FALSE(std::filesystem::exists(temp.path / "Editor.log.1"));
}

TEST_CASE("RotateLogIfLarge moves an oversized log aside instead of deleting it")
{
	const TempDir temp;
	const auto log = temp.path / "Editor.log";
	WriteFile(log, std::string(2048, 'x'));

	CHECK(RotateLogIfLarge(log, 1024));
	// The run that filled the log is the one worth reading, so it survives as .1 and
	// the live path is clear for a fresh file.
	CHECK_FALSE(std::filesystem::exists(log));
	CHECK(std::filesystem::exists(temp.path / "Editor.log.1"));
	CHECK(ReadFile(temp.path / "Editor.log.1").size() == 2048);
}

TEST_CASE("RotateLogIfLarge keeps only one previous generation")
{
	const TempDir temp;
	const auto log = temp.path / "Editor.log";
	WriteFile(temp.path / "Editor.log.1", "older generation");
	WriteFile(log, std::string(2048, 'y'));

	CHECK(RotateLogIfLarge(log, 1024));
	CHECK(ReadFile(temp.path / "Editor.log.1").size() == 2048);
	CHECK_FALSE(std::filesystem::exists(temp.path / "Editor.log.2"));
}

TEST_CASE("RotateLogIfLarge is a no-op when it cannot or should not act")
{
	const TempDir temp;
	const auto log = temp.path / "Editor.log";
	WriteFile(log, std::string(2048, 'z'));

	// A zero limit would otherwise rotate every log on every single open.
	CHECK_FALSE(RotateLogIfLarge(log, 0));
	CHECK_FALSE(RotateLogIfLarge(temp.path / "missing.log", 1024));
	CHECK_FALSE(RotateLogIfLarge({}, 1024));
	CHECK(std::filesystem::exists(log));
}
