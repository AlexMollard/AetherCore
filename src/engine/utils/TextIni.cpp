#include "utils/TextIni.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <toml++/toml.hpp>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::text
{
	namespace
	{
		std::string TomlNodeToValueString(const toml::node& node)
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
			if (const auto* const v = node.as_floating_point())
			{
				std::ostringstream out;
				out << v->get();
				return out.str();
			}
			if (const auto* const arr = node.as_array())
			{
				std::string value = "[";
				for (std::size_t i = 0; i < arr->size(); ++i)
				{
					if (i > 0)
					{
						value += ", ";
					}
					const toml::node* elem = arr->get(i);
					if (elem != nullptr)
					{
						value += TomlNodeToValueString(*elem);
					}
				}
				value += "]";
				return value;
			}
			return {};
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
				if (const auto* const childTable = node.as_table())
				{
					EmitTomlTable(*childTable, fullKey, onEntry);
					continue;
				}

				IniEntry entry;
				entry.section = sectionPrefix;
				entry.key = keyStr;
				entry.fullKey = std::move(fullKey);
				entry.value = TomlNodeToValueString(node);
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
		try
		{
			return std::stoi(std::string(value));
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	std::optional<float> ParseFloat(std::string_view value)
	{
		try
		{
			return std::stof(std::string(value));
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	void ParseToml(std::string_view text, const std::function<void(const IniEntry&)>& onEntry)
	{
		toml::table parsed;
		try
		{
			parsed = toml::parse(text);
		}
		catch (const toml::parse_error& err)
		{
			AE_ERROR(LogCategory::Engine, "TOML parse error at line {}: {}", err.source().begin.line, err.description());
			return;
		}
		EmitTomlTable(parsed, {}, onEntry);
	}
} // namespace aether::text
