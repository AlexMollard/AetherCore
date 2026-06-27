#include "utils/TomlConfig.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "io/FileSystem.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/TextIni.hpp"

namespace aether
{
	void TomlConfig::Load(std::string_view tomlText)
	{
		m_values.clear();
		m_dirty = false;
		text::ParseToml(tomlText, [this](const text::IniEntry& entry) { m_values[entry.fullKey] = entry.value; });
	}

	void TomlConfig::Save(std::ostream& out, std::string_view headerComment) const
	{
		if (!headerComment.empty())
		{
			out << "# " << headerComment << "\n";
		}

		std::string currentSection;
		for (const auto& [fullKey, rawValue]: m_values)
		{
			const auto dot = fullKey.find('.');
			const std::string_view section = dot != std::string_view::npos ? std::string_view(fullKey).substr(0, dot) : std::string_view{};
			const std::string_view key = dot != std::string_view::npos ? std::string_view(fullKey).substr(dot + 1) : std::string_view(fullKey);

			if (section != currentSection)
			{
				if (!currentSection.empty() || !headerComment.empty())
				{
					out << "\n";
				}
				currentSection = std::string(section);
				if (!section.empty())
				{
					out << "[" << section << "]\n";
				}
			}

			out << key << " = " << rawValue << "\n";
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
		if (!std::filesystem::exists(path))
		{
			return false;
		}

		std::ifstream in(path);
		if (!in.is_open())
		{
			AE_WARN(LogCategory::Engine, "Failed to open config file: {}", path.string());
			return false;
		}

		std::stringstream buffer;
		buffer << in.rdbuf();
		Load(buffer.str());
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

		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		std::ofstream out(path, std::ios::trunc);
		if (!out.is_open())
		{
			AE_WARN(LogCategory::Engine, "Failed to write config file: {}", path.string());
			return false;
		}

		Save(out, headerComment);
		return true;
	}

	bool TomlConfig::GetBool(std::string_view key, bool defaultValue) const
	{
		const auto it = m_values.find(key);
		if (it == m_values.end())
		{
			return defaultValue;
		}
		const auto parsed = text::ParseBool(it->second);
		return parsed.has_value() ? *parsed : defaultValue;
	}

	float TomlConfig::GetFloat(std::string_view key, float defaultValue) const
	{
		const auto it = m_values.find(key);
		if (it == m_values.end())
		{
			return defaultValue;
		}
		const auto parsed = text::ParseFloat(it->second);
		return parsed.has_value() ? *parsed : defaultValue;
	}

	void TomlConfig::Set(std::string_view key, bool value)
	{
		const std::string str = value ? "true" : "false";
		auto& entry = m_values[std::string(key)];
		if (entry != str)
		{
			entry = str;
			m_dirty = true;
		}
	}

	void TomlConfig::Set(std::string_view key, float value)
	{
		const std::string str = std::format("{:.2f}", value);
		auto& entry = m_values[std::string(key)];
		if (entry != str)
		{
			entry = std::move(str);
			m_dirty = true;
		}
	}

	bool TomlConfig::Has(std::string_view key) const
	{
		return m_values.find(key) != m_values.end();
	}

	void TomlConfig::Clear()
	{
		m_values.clear();
		m_dirty = false;
	}
} // namespace aether
