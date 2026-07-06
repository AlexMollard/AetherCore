#include "utils/FuzzyMatch.hpp"

#include <cctype>
#include <cstddef>

namespace aether
{
	namespace
	{
		char LowerAscii(char c)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		bool IsBoundary(char c)
		{
			return c == ' ' || c == '/' || c == '_' || c == '-' || c == '.';
		}
	} // namespace

	std::optional<int> FuzzyMatch(std::string_view needle, std::string_view haystack)
	{
		if (needle.empty())
		{
			return 0;
		}

		int score = 0;
		std::size_t hi = 0;     // haystack cursor
		int previousMatch = -2; // haystack index of the previous matched char
		bool firstMatch = true;

		for (std::size_t ni = 0; ni < needle.size(); ++ni)
		{
			const char want = LowerAscii(needle[ni]);
			bool found = false;
			for (; hi < haystack.size(); ++hi)
			{
				if (LowerAscii(haystack[hi]) == want)
				{
					found = true;
					break;
				}
			}
			if (!found)
			{
				return std::nullopt;
			}

			score += 1; // base per matched char
			if (static_cast<int>(hi) == previousMatch + 1)
			{
				score += 15; // consecutive run
			}
			if (hi == 0 || IsBoundary(haystack[hi - 1]))
			{
				score += 10; // word boundary
			}
			if (firstMatch)
			{
				score -= static_cast<int>(hi); // penalise a late first match
				firstMatch = false;
			}

			previousMatch = static_cast<int>(hi);
			++hi;
		}

		return score;
	}
} // namespace aether
