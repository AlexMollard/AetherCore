#include "utils/EngineSettings.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TextIni.hpp"

namespace aether
{
	namespace
	{
		template<class T>
		void AssignField(T& field, std::string_view raw)
		{
			if constexpr (std::is_same_v<T, bool>)
			{
				if (const auto parsed = text::ParseBool(raw))
				{
					field = *parsed;
				}
			}
			else if constexpr (std::is_same_v<T, int>)
			{
				if (const auto parsed = text::ParseInt(raw))
				{
					field = *parsed;
				}
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				if (const auto parsed = text::ParseFloat(raw))
				{
					field = *parsed;
				}
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				field = text::StripQuotes(std::string(raw));
			}
			else
			{
				static_assert(sizeof(T) == 0, "ForEachSettingField uses a field type with no AssignField branch");
			}
		}

		template<class T>
		std::string FormatField(const T& field)
		{
			if constexpr (std::is_same_v<T, bool>)
			{
				return field ? "true" : "false";
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				return "\"" + field + "\"";
			}
			else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, float>)
			{
				std::ostringstream value;
				value << field;
				return value.str();
			}
			else
			{
				static_assert(sizeof(T) == 0, "ForEachSettingField uses a field type with no FormatField branch");
			}
		}
	} // namespace

	void EngineSettingsIO::Apply(std::string_view tomlText, EngineSettings& settings, const SettingsScope scope)
	{
		text::ParseToml(tomlText,
		        [&settings, scope](const text::IniEntry& entry)
		        {
			        ForEachSettingField(settings,
			                [&entry, scope](std::string_view key, auto& field)
			                {
				                if (scope == SettingsScope::UserOverridable && IsProjectOnlySettingKey(key))
				                {
					                return;
				                }
				                if (text::ToLowerAscii(std::string(key)) == entry.fullKey)
				                {
					                AssignField(field, entry.value);
				                }
			                });
		        });
	}

	void EngineSettingsIO::Sanitize(EngineSettings& settings)
	{
		const EngineSettings defaults{};
		if (settings.window.width < 1)
		{
			settings.window.width = defaults.window.width;
		}
		if (settings.window.height < 1)
		{
			settings.window.height = defaults.window.height;
		}
		settings.app.targetFps = std::max(0.0f, settings.app.targetFps);
		settings.graphics.uiScale = std::clamp(settings.graphics.uiScale, 0.5f, 3.0f);
	}

	std::string EngineSettingsIO::Serialize(const EngineSettings& settings)
	{
		std::ostringstream out;
		out << "# AetherCore settings (TOML)\n";

		std::string currentSection;
		ForEachSettingField(settings,
		        [&](std::string_view key, const auto& field)
		        {
			        const auto [section, name] = SplitSettingKey(key);
			        if (section != currentSection)
			        {
				        currentSection = std::string(section);
				        out << "\n[" << currentSection << "]\n";
			        }
			        out << name << " = " << FormatField(field) << "\n";
		        });
		return out.str();
	}

	std::string EngineSettingsIO::SerializeOverrides(const EngineSettings& settings, const EngineSettings& base)
	{
		struct FieldLine
		{
			std::string_view section;
			std::string_view name;
			std::string value;
			bool projectOnly;
		};

		std::vector<FieldLine> current;
		std::vector<std::string> baseline;
		ForEachSettingField(settings,
		        [&](std::string_view key, const auto& field)
		        {
			        const auto [section, name] = SplitSettingKey(key);
			        current.push_back({section, name, FormatField(field), IsProjectOnlySettingKey(key)});
		        });
		ForEachSettingField(base, [&](std::string_view, const auto& field) { baseline.push_back(FormatField(field)); });

		std::ostringstream out;
		out << "# AetherCore user settings (TOML)\n";
		out << "# Overrides layered on top of the shipped EngineSettings.toml; only changed keys are stored.\n";

		std::string currentSection;
		for (std::size_t i = 0; i < current.size(); ++i)
		{
			// Project-only keys are never a user preference, so they never enter the
			// per-user file even when they differ from the base.
			if (current[i].projectOnly || current[i].value == baseline[i])
			{
				continue;
			}
			if (current[i].section != currentSection)
			{
				currentSection = std::string(current[i].section);
				out << "\n[" << currentSection << "]\n";
			}
			out << current[i].name << " = " << current[i].value << "\n";
		}
		return out.str();
	}

	std::filesystem::path EngineSettingsIO::ResolvePath(std::string_view fileName)
	{
		std::filesystem::path requested(fileName);
		if (requested.is_absolute())
		{
			return requested;
		}

		const auto exeDir = io::PlatformPaths::GetExecutableDir();
		std::error_code ec;
		const auto cwd = std::filesystem::current_path(ec);

		std::vector<std::filesystem::path> candidates;
		if (!exeDir.empty())
		{
			candidates.push_back(exeDir / "data" / "config" / requested);
			candidates.push_back(exeDir / "config" / requested);
			candidates.push_back(exeDir / requested);
		}
		if (!ec)
		{
			candidates.push_back(cwd / "data" / "config" / requested);
			candidates.push_back(cwd / ".." / "data" / "config" / requested);
			candidates.push_back(cwd / ".." / ".." / "data" / "config" / requested);
			candidates.push_back(cwd / "config" / requested);
			candidates.push_back(cwd / ".." / "config" / requested);
			candidates.push_back(cwd / ".." / ".." / "config" / requested);
			candidates.push_back(cwd / requested);
		}

		for (const auto& candidate: candidates)
		{
			if (io::file_util::Exists(candidate))
			{
				return candidate;
			}
		}
		return candidates.empty() ? requested : candidates.front();
	}

	LoadedEngineSettings EngineSettingsIO::LoadLayered(std::string_view shippedFile, const std::filesystem::path& projectFile, std::string_view userFile)
	{
		AE_PROFILE_ZONE();
		LoadedEngineSettings result;

		const auto shippedPath = ResolvePath(shippedFile);
		if (auto text = io::file_util::ReadText(shippedPath))
		{
			Apply(*text, result.values);
			AE_INFO(LogCategory::Engine, "Shipped settings loaded from {}", shippedPath.string());
		}
		else
		{
			AE_INFO(LogCategory::Engine, "No shipped settings file at {}; using compiled-in defaults.", shippedPath.string());
		}

		if (!projectFile.empty())
		{
			if (auto text = io::file_util::ReadText(projectFile))
			{
				Apply(*text, result.values);
				AE_INFO(LogCategory::Engine, "Project settings loaded from {}", projectFile.string());
			}
		}

		Sanitize(result.values);
		result.base = result.values;

		if (const auto userDir = io::PlatformPaths::GetUserConfigDir(); !userDir.empty())
		{
			const auto userPath = userDir / userFile;
			if (auto text = io::file_util::ReadText(userPath))
			{
				Apply(*text, result.values, SettingsScope::UserOverridable);
				AE_INFO(LogCategory::Engine, "User settings overrides loaded from {}", userPath.string());
			}
		}
		Sanitize(result.values);

		const auto& v = result.values;
		AE_INFO(LogCategory::Engine,
		        "Settings resolved ({}x{}, VSync={}, FXAA={}, AsyncCompute={}, TargetFPS={})",
		        v.window.width,
		        v.window.height,
		        v.graphics.vsync ? "on" : "off",
		        v.graphics.fxaa ? "on" : "off",
		        v.graphics.asyncCompute ? "on" : "off",
		        v.app.targetFps);
		return result;
	}

	EngineSettings EngineSettingsIO::LoadOrCreate(std::string_view shippedFile, const std::filesystem::path& projectFile, std::string_view userFile)
	{
		return LoadLayered(shippedFile, projectFile, userFile).values;
	}

	void EngineSettingsIO::SaveUserOverrides(const EngineSettings& settings, const EngineSettings& base, std::string_view userFile)
	{
		AE_PROFILE_ZONE();
		const auto userDir = io::PlatformPaths::GetUserConfigDir();
		if (userDir.empty())
		{
			AE_WARN(LogCategory::Engine, "No user config directory available; settings not saved.");
			return;
		}

		const auto path = userDir / userFile;
		const std::string text = SerializeOverrides(settings, base);

		if (auto result = io::file_util::WriteText(path, text); !result)
		{
			AE_WARN(LogCategory::Engine, "Failed to write user settings file: {} - {}", path.string(), result.error().message);
			return;
		}
		AE_INFO(LogCategory::Engine, "User settings saved to {}", path.string());
	}
} // namespace aether
