#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace aether::utils
{
	// Case-insensitive ASCII comparison of two strings.
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

	// Levenshtein distance – used for fuzzy "Did you mean?" suggestions.
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
