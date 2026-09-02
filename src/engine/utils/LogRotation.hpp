#pragma once

#include <cstdint>
#include <filesystem>

namespace aether
{
	// Logs open in append mode, so without this a log file is never anything but
	// longer: an editor log had reached 8 MB and a test log 22 MB, spanning every run
	// since the file was created. Size is a better bound than run count because runs
	// vary from seconds to hours.
	inline constexpr std::uintmax_t kDefaultMaxLogBytes = 8ull * 1024ull * 1024ull;

	// If `path` is at least `maxBytes` long, move it aside to "<path>.1" - replacing any
	// previous generation - so the next open starts a fresh file. Returns whether it
	// rotated.
	//
	// Called when the log is opened rather than on every write, which keeps the check
	// off the logging path entirely. The ceiling that buys: one run can still push a
	// single file past `maxBytes`, because the size is only tested at startup. Bound a
	// run's own output too if a single session ever gets long enough to matter.
	bool RotateLogIfLarge(const std::filesystem::path& path, std::uintmax_t maxBytes);
} // namespace aether
