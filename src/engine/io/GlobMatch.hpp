#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace aether::io
{
	// Compile a glob pattern to an ECMAScript regex. Shared by every file backend
	// (pak, loose directory, overlay) so a project globs identically however it is
	// mounted. `*` matches within a path segment, `**` spans segments, `**/` is an
	// optional directory prefix, `?` matches one non-slash char; regex metacharacters
	// are escaped.
	inline std::string GlobToRegex(std::string_view pattern)
	{
		std::string regex;
		regex.reserve(pattern.size() * 2 + 2);
		regex += '^';

		for (std::size_t i = 0; i < pattern.size(); ++i)
		{
			const char c = pattern[i];
			if (c == '*')
			{
				const bool isDoubleStar = (i + 1 < pattern.size() && pattern[i + 1] == '*');
				if (isDoubleStar)
				{
					const bool followedBySlash = (i + 2 < pattern.size() && pattern[i + 2] == '/');
					if (followedBySlash)
					{
						regex += "(?:.*/)?";
						i += 2;
					}
					else
					{
						regex += ".*";
						++i;
					}
				}
				else
				{
					regex += "[^/]*";
				}
				continue;
			}
			if (c == '?')
			{
				regex += "[^/]";
				continue;
			}
			if (c == '.' || c == '^' || c == '$' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '\\')
			{
				regex += '\\';
			}
			regex += c;
		}

		regex += '$';
		return regex;
	}
} // namespace aether::io
