#pragma once

#include <optional>
#include <string_view>

namespace aether
{
	// Case-insensitive subsequence fuzzy match used by the command palette and
	// other search fields. Returns a score (higher = better) when every character
	// of `needle` appears in `haystack` in order, or std::nullopt when it does not.
	//
	// Scoring favours matches that a human would rank first: consecutive runs and
	// matches at word boundaries (start, or after a space / '/' / '_' / '-') score
	// higher; leading gaps score lower. An empty needle matches everything with
	// score 0 (so an empty query lists all entries in their natural order).
	[[nodiscard]] std::optional<int> FuzzyMatch(std::string_view needle, std::string_view haystack);
} // namespace aether
