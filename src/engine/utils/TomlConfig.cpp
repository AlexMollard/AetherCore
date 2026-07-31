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
		// A TOML basic string with the two characters that must not appear raw inside one
		// escaped. Both Set and the re-quoting path below go through this: a Windows path
		// round-tripped as `"D:\AetherCore\..."` is not merely ugly, `\A` is an invalid
		// escape and the whole document stops parsing.
		std::string QuoteTomlString(std::string_view value)
		{
			std::string quoted = "\"";
			for (const char c: value)
			{
				if (c == '\\' || c == '"')
				{
					quoted += '\\';
				}
				quoted += c;
			}
			quoted += '"';
			return quoted;
		}

		// loaded (not Set), we must re-quote bare strings to produce valid TOML.
		bool IsBareStringValue(std::string_view value)
		{
			if (value.empty())
			{
				return true;
			}
			if (value.front() == '"' || value.front() == '\'')
			{
				return false;
			}
			if (value.front() == '[')
			{
				return false;
			}
			if (value == "true" || value == "false")
			{
				return false;
			}
			char* end = nullptr;
			std::strtod(value.data(), &end);
			return end != value.data() + value.size();
		}
	} // namespace

	bool TomlConfig::Load(std::string_view tomlText)
	{
		m_values.clear();
		m_dirty = false;
		return text::ParseToml(tomlText, [this](const text::IniEntry& entry) { m_values[entry.fullKey] = entry.value; });
	}

	namespace
	{
		void WriteEntry(std::ostream& out, std::string_view key, std::string_view rawValue)
		{
			out << key << " = ";
			if (IsBareStringValue(rawValue))
			{
				out << QuoteTomlString(rawValue);
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
		bool wroteAny = !headerComment.empty();
		for (const auto& [fullKey, rawValue]: m_values)
		{
			if (fullKey.contains('.'))
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
					return Load(text);
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

		return Load(*text);
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

	bool TomlConfig::LoadFromPath(const std::filesystem::path& path)
	{
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			return false;
		}
		return Load(*text);
	}

	bool TomlConfig::SaveToPath(const std::filesystem::path& path, std::string_view headerComment) const
	{
		std::ostringstream buffer;
		Save(buffer, headerComment);
		if (auto result = io::file_util::WriteText(path, buffer.str()); !result)
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
			entry = str;
			m_dirty = true;
		}
	}

	void TomlConfig::Set(std::string_view key, std::string_view value)
	{
		const std::string lowerKey = text::ToLowerAscii(std::string(key));
		std::string str = QuoteTomlString(value);
		auto& entry = m_values[lowerKey];
		if (entry != str)
		{
			entry = std::move(str);
			m_dirty = true;
		}
	}

	bool TomlConfig::Has(std::string_view key) const
	{
		return m_values.contains(text::ToLowerAscii(std::string(key)));
	}

	void TomlConfig::Clear()
	{
		m_values.clear();
		m_dirty = false;
	}
} // namespace aether
