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
#include "utils/TomlParse.hpp"
#include <toml++/toml.hpp>

namespace aether
{
	namespace
	{
		// Use the TOML formatter for string escaping, including control characters.
		std::string QuoteTomlString(std::string_view value)
		{
			std::ostringstream out;
			out << toml::toml_formatter{toml::value{std::string(value)}};
			return out.str();
		}

	} // namespace

	bool TomlConfig::Load(std::string_view tomlText)
	{
		m_values.clear();
		m_serialized_values.clear();
		m_dirty = false;
		return text::ParseToml(tomlText, [this](const text::IniEntry& entry)
		        {
			        m_values[entry.fullKey] = entry.value;
			        m_serialized_values[entry.fullKey] = entry.tomlValue;
		        });
	}


	void TomlConfig::Save(std::ostream& out, std::string_view headerComment) const
	{
		if (!headerComment.empty())
		{
			out << "# " << headerComment << "\n";
		}

		// Section-less (top-level) keys MUST be emitted before any [section]
		bool wroteAny = !headerComment.empty();
		for (const auto& [fullKey, rawValue]: m_serialized_values)
		{
			if (fullKey.contains('.'))
			{
				continue;
			}
			out << fullKey << " = " << rawValue << "\n";
			wroteAny = true;
		}

		std::string currentSection;
		for (const auto& [fullKey, rawValue]: m_serialized_values)
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

			out << key << " = " << rawValue << "\n";
		}
	}

	bool TomlConfig::LoadFile(std::string_view fileName)
	{
#if defined(AETHERCORE_WEB_RUNTIME)
		return false;
#else
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
#endif
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
#if defined(AETHERCORE_WEB_RUNTIME)
		return false;
#else
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
#endif
	}

	bool TomlConfig::LoadFromPath(const std::filesystem::path& path)
	{
#if defined(AETHERCORE_WEB_RUNTIME)
		auto text = io::FileSystem::ReadFileText(path.generic_string());
		return text && Load(*text);
#else
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			return false;
		}
		return Load(*text);
#endif
	}

	bool TomlConfig::SaveToPath(const std::filesystem::path& path, std::string_view headerComment) const
	{
#if defined(AETHERCORE_WEB_RUNTIME)
		std::ostringstream buffer;
		Save(buffer, headerComment);
		return io::FileSystem::WriteFileText(path.generic_string(), buffer.str()).has_value();
#else
		std::ostringstream buffer;
		Save(buffer, headerComment);
		if (auto result = io::file_util::WriteText(path, buffer.str()); !result)
		{
			AE_WARN(LogCategory::Engine, "Failed to write config file: {} - {}", path.string(), result.error().message);
			return false;
		}
		return true;
#endif
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

	std::optional<double> TomlConfig::GetNumber(std::string_view key) const
	{
		const auto it = m_serialized_values.find(text::ToLowerAscii(std::string(key)));
		if (it == m_serialized_values.end())
		{
			return std::nullopt;
		}
		const auto table = ParseTomlDocument("value = " + it->second);
		if (!table)
		{
			return std::nullopt;
		}
		if (const auto* integer = (*table)["value"].as_integer())
		{
			return static_cast<double>(integer->get());
		}
		if (const auto* floating = (*table)["value"].as_floating_point())
		{
			return floating->get();
		}
		return std::nullopt;
	}

	std::string TomlConfig::GetRawString(std::string_view key, std::string_view defaultValue) const
	{
		const auto it = m_values.find(text::ToLowerAscii(std::string(key)));
		return it == m_values.end() ? std::string(defaultValue) : it->second;
	}

	std::string TomlConfig::GetString(std::string_view key, std::string_view defaultValue) const
	{
		const auto it = m_values.find(text::ToLowerAscii(std::string(key)));
		if (it == m_values.end())
		{
			return std::string(defaultValue);
		}
		return it->second.starts_with("uid://") ? io::FileSystem::ResolveAssetPath(it->second) : it->second;
	}

	void TomlConfig::SetValue(std::string_view key, std::string value, std::string serialized)
	{
		const std::string lowerKey = text::ToLowerAscii(std::string(key));
		auto& decoded = m_values[lowerKey];
		auto& encoded = m_serialized_values[lowerKey];
		if (decoded != value || encoded != serialized)
		{
			decoded = std::move(value);
			encoded = std::move(serialized);
			m_dirty = true;
		}
	}

	void TomlConfig::Set(std::string_view key, bool value)
	{
		const std::string str = value ? "true" : "false";
		SetValue(key, str, str);
	}

	void TomlConfig::Set(std::string_view key, float value)
	{
		std::string str = std::format("{}", value);
		if (str.find_first_of(".eE") == std::string::npos && str != "inf" && str != "-inf" && str != "nan" && str != "-nan")
		{
			str += ".0";
		}
		SetValue(key, str, str);
	}

	void TomlConfig::Set(std::string_view key, int value)
	{
		const std::string str = std::to_string(value);
		SetValue(key, str, str);
	}

	void TomlConfig::Set(std::string_view key, std::string_view value)
	{
		const std::string reference = io::FileSystem::AssetReference(value);
		SetValue(key, reference, QuoteTomlString(reference));
	}

	bool TomlConfig::Has(std::string_view key) const
	{
		return m_values.contains(text::ToLowerAscii(std::string(key)));
	}

	void TomlConfig::Erase(std::string_view key)
	{
		const std::string lowerKey = text::ToLowerAscii(std::string(key));
		m_serialized_values.erase(lowerKey);
		if (m_values.erase(lowerKey) > 0)
		{
			m_dirty = true;
		}
	}

	void TomlConfig::Clear()
	{
		m_values.clear();
		m_serialized_values.clear();
		m_dirty = false;
	}
} // namespace aether
