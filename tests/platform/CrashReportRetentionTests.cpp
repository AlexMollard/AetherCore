#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/CrashReportRetention.hpp"

using aether::platform::PruneCrashReports;

namespace
{
	struct TempDir
	{
		std::filesystem::path path;

		TempDir()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			path = std::filesystem::temp_directory_path() / ("aether_crash_retention_" + std::to_string(stamp));
			std::filesystem::create_directories(path);
		}

		~TempDir()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	// Written oldest-first with an explicit mtime, because the prune orders by write
	// time and creating files in a loop can land several in the same filesystem tick.
	void WriteReport(const std::filesystem::path& directory, const std::string& stem, const std::vector<std::string>& extensions, int ageInMinutes)
	{
		for (const std::string& extension: extensions)
		{
			const std::filesystem::path file = directory / (stem + extension);
			std::ofstream{file} << "x";
			std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(ageInMinutes));
		}
	}

	bool Exists(const std::filesystem::path& directory, const std::string& name)
	{
		return std::filesystem::exists(directory / name);
	}
} // namespace

TEST_CASE("PruneCrashReports keeps the newest reports and removes the rest")
{
	const TempDir temp;
	WriteReport(temp.path, "Editor_old", {".dmp", ".txt"}, 30);
	WriteReport(temp.path, "Editor_middle", {".dmp", ".txt"}, 20);
	WriteReport(temp.path, "Editor_new", {".dmp", ".txt"}, 10);

	CHECK(PruneCrashReports(temp.path, "Editor_", 1) == 4);

	CHECK(Exists(temp.path, "Editor_new.dmp"));
	CHECK(Exists(temp.path, "Editor_new.txt"));
	CHECK_FALSE(Exists(temp.path, "Editor_old.dmp"));
	CHECK_FALSE(Exists(temp.path, "Editor_middle.dmp"));
}

TEST_CASE("PruneCrashReports removes every file of a report together")
{
	const TempDir temp;
	WriteReport(temp.path, "Editor_a", {".dmp", ".txt", ".log"}, 30);
	WriteReport(temp.path, "Editor_b", {".dmp"}, 10);

	CHECK(PruneCrashReports(temp.path, "Editor_", 1) == 3);
	CHECK_FALSE(Exists(temp.path, "Editor_a.dmp"));
	CHECK_FALSE(Exists(temp.path, "Editor_a.txt"));
	CHECK_FALSE(Exists(temp.path, "Editor_a.log"));
	CHECK(Exists(temp.path, "Editor_b.dmp"));
}

TEST_CASE("PruneCrashReports never touches files it did not write")
{
	const TempDir temp;
	WriteReport(temp.path, "Editor_a", {".dmp"}, 30);
	WriteReport(temp.path, "Editor_b", {".dmp"}, 20);
	WriteReport(temp.path, "notes", {".txt"}, 90);
	WriteReport(temp.path, "GameRuntime_a", {".dmp"}, 90);

	CHECK(PruneCrashReports(temp.path, "Editor_", 1) == 1);
	CHECK(Exists(temp.path, "notes.txt"));
	CHECK(Exists(temp.path, "GameRuntime_a.dmp"));
	CHECK(Exists(temp.path, "Editor_b.dmp"));
}

TEST_CASE("PruneCrashReports does nothing when under the limit or misused")
{
	const TempDir temp;
	WriteReport(temp.path, "Editor_a", {".dmp"}, 30);
	WriteReport(temp.path, "Editor_b", {".dmp"}, 20);

	CHECK(PruneCrashReports(temp.path, "Editor_", 10) == 0);
	CHECK(PruneCrashReports(temp.path, "Editor_", 2) == 0);
	// An empty prefix would otherwise match every file in the directory.
	CHECK(PruneCrashReports(temp.path, "", 0) == 0);
	CHECK(PruneCrashReports(temp.path / "missing", "Editor_", 0) == 0);
	CHECK(Exists(temp.path, "Editor_a.dmp"));
	CHECK(Exists(temp.path, "Editor_b.dmp"));
}
