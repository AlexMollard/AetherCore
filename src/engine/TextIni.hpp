#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace aether::text
{
	struct IniEntry
	{
		std::string section;
		std::string key;
		std::string value;
		std::string fullKey;
	};

	std::string TrimAscii(std::string value);
	std::string ToLowerAscii(std::string value);
	std::string StripQuotes(std::string value);

	std::optional<bool> ParseBool(std::string_view value);
	std::optional<int> ParseInt(std::string_view value);
	std::optional<float> ParseFloat(std::string_view value);

	template<std::size_t N>
	std::optional<std::array<float, N>> ParseFloatArray(std::string value)
	{
		value = TrimAscii(std::move(value));
		if (value.size() >= 2 && value.front() == '[' && value.back() == ']')
		{
			value = TrimAscii(value.substr(1, value.size() - 2));
		}

		for (char& c: value)
		{
			if (c == ',')
			{
				c = ' ';
			}
		}

		std::array<float, N> out{};
		std::size_t parsedCount = 0;
		std::string token;
		std::size_t start = 0;
		while (start <= value.size())
		{
			const std::size_t end = value.find(' ', start);
			token = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
			token = TrimAscii(std::move(token));
			if (!token.empty())
			{
				if (parsedCount >= N)
				{
					return std::nullopt;
				}
				const auto parsed = ParseFloat(token);
				if (!parsed)
				{
					return std::nullopt;
				}
				out[parsedCount++] = *parsed;
			}

			if (end == std::string::npos)
			{
				break;
			}
			start = end + 1;
		}

		if (parsedCount != N)
		{
			return std::nullopt;
		}

		return out;
	}

	void ParseToml(std::string_view text, const std::function<void(const IniEntry&)>& onEntry);
} // namespace aether::text
