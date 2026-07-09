#include "utils/TomlConfig.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>

#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/TextIni.hpp"

namespace aether
{
	namespace
	{
		// ParseToml strips quotes from string values. When saving a value that was
		// loaded (not Set), we must re-quote bare strings to produce valid TOML.
		// Booleans, numbers, and arrays are left as-is.
		bool IsBareStringValue(std::string_view value)
		{
			if (value.empty())
			{
				return true;
			}
			if (value.front() == '"' || value.front() == '\'')
			{
				return false; // already quoted
			}
			if (value.front() == '[')
			{
				return false; // array
			}
			if (value == "true" || value == "false")
			{
				return false; // boolean
			}
			// Numbers
			char* end = nullptr;
			std::strtod(value.data(), &end);
			if (end == value.data() + value.size())
			{
				return false;
			}
			return true; // bare word → needs quotes
		}
	} // namespace

	void TomlConfig::Load(std::string_view tomlText)
	{
		m_values.clear();
		m_dirty = false;
		text::ParseToml(tomlText, [this](const text::IniEntry& entry) { m_values[entry.fullKey] = entry.value; });
	}

	namespace
	{
		void WriteEntry(std::ostream& out, std::string_view key, std::string_view rawValue)
		{
			out << key << " = ";
			if (IsBareStringValue(rawValue))
			{
				out << '"' << rawValue << '"';
			}
			else
			{
				out << rawValue;
			}
			out << "\n";
		}
	} // namespace

	void TomlConfig::Save(std::ostream& out, std::string_view headerComment) const
	{
		if (!headerComment.empty())
		{
			out << "# " << headerComment << "\n";
		}

		// Section-less (top-level) keys MUST be emitted before any [section]
		// header. In TOML a bare key that appears after a header belongs to that
		// table, so a top-level key written mid-file would be silently reparented
		// under the preceding section on reload - and if a correctly-sectioned key
		// of the same name is then written, the file gains a duplicate key and the
		// whole config is rejected on the next parse. m_values is sorted, so a
		// top-level key does not necessarily precede every sectioned key; walk the
		// top-level keys in an explicit first pass to guarantee correct ordering.
		bool wroteAny = !headerComment.empty();
		for (const auto& [fullKey, rawValue]: m_values)
		{
			if (fullKey.find('.') != std::string::npos)
			{
				continue;
			}
			WriteEntry(out, fullKey, rawValue);
			wroteAny = true;
		}

		std::string currentSection;
		for (const auto& [fullKey, rawValue]: m_values)
		{
			const auto dot = fullKey.find('.');
			if (dot == std::string::npos)
			{
				continue;
			}
			const std::string_view section = std::string_view(fullKey).substr(0, dot);
			const std::string_view key = std::string_view(fullKey).substr(dot + 1);

			if (section != currentSection)
			{
				if (wroteAny)
				{
					out << "\n";
				}
				currentSection = std::string(section);
				out << "[" << section << "]\n";
				wroteAny = true;
			}

			WriteEntry(out, key, rawValue);
		}
	}

	bool TomlConfig::LoadFile(std::string_view fileName)
	{
		const std::string virtualPath = std::format("config://{}.toml", fileName);

		if (io::FileSystem::IsInitialized())
		{
			try
			{
				if (io::FileSystem::Exists(virtualPath))
				{
					AE_EXPECT_OR_THROW(bytes, io::FileSystem::ReadFile(virtualPath));
					std::string text;
					text.resize(bytes.size());
					for (std::size_t i = 0; i < bytes.size(); ++i)
					{
						text[i] = static_cast<char>(bytes[i]);
					}
					Load(text);
					return true;
				}
			}
			catch (const std::exception& e)
			{
				AE_WARN(LogCategory::Engine, "VFS read failed ({}): {}", virtualPath, e.what());
			}
		}

		const std::string tomlName = std::format("{}.toml", fileName);
		const std::filesystem::path path = EngineSettingsIO::ResolvePath(tomlName);
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			return false;
		}

		Load(*text);
		return true;
	}

	bool TomlConfig::SaveIfDirty(std::string_view fileName, std::string_view headerComment)
	{
		if (!m_dirty)
		{
			return false;
		}
		if (!SaveFile(fileName, headerComment))
		{
			return false;
		}
		m_dirty = false;
		return true;
	}

	bool TomlConfig::SaveFile(std::string_view fileName, std::string_view headerComment) const
	{
		const std::string tomlName = std::format("{}.toml", fileName);
		const std::filesystem::path path = EngineSettingsIO::ResolvePath(tomlName);

		std::ostringstream buffer;
		Save(buffer, headerComment);

		auto result = io::file_util::WriteText(path, buffer.str());
		if (!result)
		{
			AE_WARN(LogCategory::Engine, "Failed to write config file: {} - {}", path.string(), result.error().message);
			return false;
		}

		return true;
	}

	bool TomlConfig::GetBool(std::string_view key, bool defaultValue) const
	{
		const auto it = m_values.find(text::ToLowerAscii(std::string(key)));
		if (it == m_values.end())
		{
			return defaultValue;
		}
		const auto parsed = text::ParseBool(it->second);
		return parsed.has_value() ? *parsed : defaultValue;
	}

	float TomlConfig::GetFloat(std::string_view key, float defaultValue) const
	{
		const auto it = m_values.find(text::ToLowerAscii(std::string(key)));
		if (it == m_values.end())
		{
			return defaultValue;
		}
		const auto parsed = text::ParseFloat(it->second);
		return parsed.has_value() ? *parsed : defaultValue;
	}

	std::string TomlConfig::GetString(std::string_view key, std::string_view defaultValue) const
	{
		const auto it = m_values.find(text::ToLowerAscii(std::string(key)));
		if (it == m_values.end())
		{
			return std::string(defaultValue);
		}
		return text::StripQuotes(it->second);
	}

	void TomlConfig::Set(std::string_view key, bool value)
	{
		const std::string lowerKey = text::ToLowerAscii(std::string(key));
		const std::string str = value ? "true" : "false";
		auto& entry = m_values[lowerKey];
		if (entry != str)
		{
			entry = str;
			m_dirty = true;
		}
	}

	void TomlConfig::Set(std::string_view key, float value)
	{
		const std::string lowerKey = text::ToLowerAscii(std::string(key));
		const std::string str = std::format("{:.2f}", value);
		auto& entry = m_values[lowerKey];
		if (entry != str)
		{
			entry = std::move(str);
			m_dirty = true;
		}
	}

	void TomlConfig::Set(std::string_view key, std::string_view value)
	{
		const std::string lowerKey = text::ToLowerAscii(std::string(key));
		std::string str = "\"";
		for (const char c: value)
		{
			if (c == '\\' || c == '"')
			{
				str += '\\';
			}
			str += c;
		}
		str += '"';
		auto& entry = m_values[lowerKey];
		if (entry != str)
		{
			entry = std::move(str);
			m_dirty = true;
		}
	}

	bool TomlConfig::Has(std::string_view key) const
	{
		return m_values.find(text::ToLowerAscii(std::string(key))) != m_values.end();
	}

	void TomlConfig::Clear()
	{
		m_values.clear();
		m_dirty = false;
	}
} // namespace aether
