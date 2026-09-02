#include "utils/LogRotation.hpp"

#include <system_error>

namespace aether
{
	bool RotateLogIfLarge(const std::filesystem::path& path, const std::uintmax_t maxBytes)
	{
		if (path.empty() || maxBytes == 0)
		{
			return false;
		}

		std::error_code error;
		const std::uintmax_t size = std::filesystem::file_size(path, error);
		if (error || size < maxBytes)
		{
			return false;
		}

		// Rename rather than delete, so the run that filled the log is still readable
		// after rotation - that run is usually the one worth reading.
		std::filesystem::path previous = path;
		previous += ".1";
		std::filesystem::remove(previous, error);
		std::filesystem::rename(path, previous, error);
		if (error)
		{
			// A locked or unwritable file must not stop the logger from starting; the
			// log simply keeps growing until the next run can rotate it.
			return false;
		}
		return true;
	}
} // namespace aether
