#include "platform/CrashReportRetention.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace aether::platform
{
	std::size_t PruneCrashReports(const std::filesystem::path& directory, std::string_view filePrefix, std::size_t keep)
	{
		std::error_code error;
		if (filePrefix.empty() || !std::filesystem::is_directory(directory, error) || error)
		{
			return 0;
		}

		const std::string prefix(filePrefix);
		std::map<std::string, std::vector<std::filesystem::path>> reports;
		std::map<std::string, std::filesystem::file_time_type> newest;

		std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error);
		if (error)
		{
			return 0;
		}
		for (const auto& entry: it)
		{
			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError)
			{
				continue;
			}
			const std::string stem = entry.path().stem().string();
			if (!stem.starts_with(prefix))
			{
				continue;
			}
			const auto written = std::filesystem::last_write_time(entry.path(), entryError);
			if (entryError)
			{
				continue;
			}
			reports[stem].push_back(entry.path());
			const auto known = newest.find(stem);
			if (known == newest.end() || written > known->second)
			{
				newest[stem] = written;
			}
		}

		if (reports.size() <= keep)
		{
			return 0;
		}

		std::vector<std::pair<std::filesystem::file_time_type, std::string>> byAge;
		byAge.reserve(newest.size());
		for (const auto& [stem, written]: newest)
		{
			byAge.emplace_back(written, stem);
		}
		// Newest first, breaking ties on the stem so the outcome does not depend on
		// directory order when several reports share a timestamp.
		std::sort(byAge.begin(), byAge.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });

		std::size_t removed = 0;
		for (std::size_t i = keep; i < byAge.size(); ++i)
		{
			for (const auto& file: reports[byAge[i].second])
			{
				std::error_code removeError;
				if (std::filesystem::remove(file, removeError))
				{
					++removed;
				}
			}
		}
		return removed;
	}
} // namespace aether::platform
