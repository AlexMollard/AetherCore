#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace aether::platform
{
	// How many crash reports to keep. Deliberately small: a minidump is ~20 MB and
	// only the newest few are ever opened, but nothing pruned them, so a working
	// directory had reached 700 MB of dumps nobody would read.
	inline constexpr std::size_t kDefaultCrashReportsToKeep = 10;

	// Delete all but the newest `keep` crash reports in `directory`, returning how many
	// FILES were removed.
	//
	// A report is every file sharing a stem, so a dump and its side files are kept or
	// dropped together - half a report is worse than none. Only files whose stem begins
	// with `filePrefix` are considered, so anything else parked in the folder is left
	// alone. Never throws: it runs at startup and must not be able to take the process
	// down, and a file it cannot remove is simply skipped.
	std::size_t PruneCrashReports(const std::filesystem::path& directory, std::string_view filePrefix, std::size_t keep);
} // namespace aether::platform
