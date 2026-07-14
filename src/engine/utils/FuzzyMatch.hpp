#pragma once

#include <optional>
#include <string_view>

namespace aether
{
	[[nodiscard]] std::optional<int> FuzzyMatch(std::string_view needle, std::string_view haystack);
}
