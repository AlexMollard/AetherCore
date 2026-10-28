#include "utils/TextIni.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include "utils/TomlParse.hpp"
#include <sstream>
#include <toml++/toml.hpp>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::text
{
	namespace
	{
		std::string TomlNodeToValueString(const toml::node& node, const std::string& serialized)
		{
			if (const auto* const v = node.as_string())
			{
				return std::string(v->get());
			}
			if (const auto* const v = node.as_boolean())
			{
				return v->get() ? "true" : "false";
			}
			if (const auto* const v = node.as_integer())
			{
				return std::to_string(v->get());
			}
			return serialized;
		}

		void EmitTomlTable(const toml::table& table, const std::string& sectionPrefix, const std::function<void(const IniEntry&)>& onEntry)
		{
			for (const auto& [key, node]: table)
			{
				const std::string keyStr = ToLowerAscii(std::string(key.str()));
				std::string fullKey = sectionPrefix;
				if (!fullKey.empty())
				{
					fullKey += '.';
				}
				fullKey += keyStr;
				if (const auto* const childTable = node.as_table(); childTable != nullptr && !childTable->is_inline())
				{
					EmitTomlTable(*childTable, fullKey, onEntry);
					continue;
				}

				IniEntry entry;
				entry.section = sectionPrefix;
				entry.key = keyStr;
				entry.fullKey = std::move(fullKey);
				std::ostringstream serialized;
				serialized << toml::toml_formatter{node};
				entry.tomlValue = serialized.str();
				entry.value = TomlNodeToValueString(node, entry.tomlValue);
				onEntry(entry);
			}
		}
	} // namespace

	std::string TrimAscii(std::string value)
	{
		auto isSpace = [](const unsigned char c)
		{
			return std::isspace(c) != 0;
		};

		while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
		{
			value.erase(value.begin());
		}
		while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
		{
			value.pop_back();
		}
		return value;
	}

	std::string ToLowerAscii(std::string value)
	{
		std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	}

	std::string StripQuotes(std::string value)
	{
		if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
		{
			value = value.substr(1, value.size() - 2);
		}
		return value;
	}

	std::optional<bool> ParseBool(std::string_view value)
	{
		const std::string lower = ToLowerAscii(std::string(value));
		if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
		{
			return true;
		}
		if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
		{
			return false;
		}
		return std::nullopt;
	}

	std::optional<int> ParseInt(std::string_view value)
	{
#if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
		int number{};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
		return parsed.ec == std::errc{} ? std::optional<int>{number} : std::nullopt;
#else
		try
		{
			return std::stoi(std::string(value));
		}
		catch (...)
		{
			return std::nullopt;
		}
#endif
	}

	std::optional<float> ParseFloat(std::string_view value)
	{
#if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
		float number{};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
		return parsed.ec == std::errc{} ? std::optional<float>{number} : std::nullopt;
#else
		try
		{
			return std::stof(std::string(value));
		}
		catch (...)
		{
			return std::nullopt;
		}
#endif
	}

	bool ParseToml(std::string_view text, const std::function<void(const IniEntry&)>& onEntry)
	{
		auto parsed = ParseTomlDocument(text);
		if (!parsed)
		{
			AE_ERROR(LogCategory::Engine, "TOML parse error: {}", parsed.error().message);
			return false;
		}
		EmitTomlTable(*parsed, {}, onEntry);
		return true;
	}
} // namespace aether::text
