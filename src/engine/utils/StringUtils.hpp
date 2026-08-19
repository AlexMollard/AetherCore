#pragma once

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace aether::utils
{
	// A span of seconds as a short human phrase: "45 sec", "12 min", "3 hr", "2 days",
	// "3 weeks". Deliberately unframed, so one rule serves both "3 hr ago" and "3 hr ahead
	// of the saved scene". Negative spans read as zero.
	inline std::string DurationLabel(const long long seconds)
	{
		const long long span = std::max<long long>(seconds, 0);
		if (span < 60)
		{
			return std::format("{} sec", span);
		}
		const long long minutes = span / 60;
		if (minutes < 60)
		{
			return std::format("{} min", minutes);
		}
		const long long hours = minutes / 60;
		if (hours < 24)
		{
			return std::format("{} hr", hours);
		}
		const long long days = hours / 24;
		if (days < 7)
		{
			return std::format("{} day{}", days, days == 1 ? "" : "s");
		}
		const long long weeks = days / 7;
		return std::format("{} week{}", weeks, weeks == 1 ? "" : "s");
	}

	// Trim leading/trailing characters in `ws` (default ASCII whitespace) from a view.
	inline std::string_view TrimView(std::string_view s, std::string_view ws = " \t\r\n")
	{
		const auto first = s.find_first_not_of(ws);
		if (first == std::string_view::npos)
		{
			return {};
		}
		const auto last = s.find_last_not_of(ws);
		return s.substr(first, last - first + 1);
	}

	inline bool IEq(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			{
				return false;
			}
		}
		return true;
	}

	inline int Levenshtein(std::string_view a, std::string_view b)
	{
		const std::size_t m = a.size();
		const std::size_t n = b.size();
		if (m == 0)
		{
			return static_cast<int>(n);
		}
		if (n == 0)
		{
			return static_cast<int>(m);
		}

		std::vector<int> prev(n + 1);
		std::vector<int> curr(n + 1);
		for (std::size_t j = 0; j <= n; ++j)
		{
			prev[j] = static_cast<int>(j);
		}

		for (std::size_t i = 1; i <= m; ++i)
		{
			curr[0] = static_cast<int>(i);
			for (std::size_t j = 1; j <= n; ++j)
			{
				const int cost = (std::tolower(static_cast<unsigned char>(a[i - 1])) == std::tolower(static_cast<unsigned char>(b[j - 1]))) ? 0 : 1;
				curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
			}
			std::swap(prev, curr);
		}
		return prev[n];
	}
} // namespace aether::utils
