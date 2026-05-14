#include "utils/EngineSettings.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"
#include "utils/TextIni.hpp"

namespace aether
{
	namespace
	{
		void ParseSettingsText(std::string_view text, EngineSettings& settings)
		{
			text::ParseToml(text,
			        [&settings](const text::IniEntry& entry)
			        {
				        if (entry.fullKey == "window.width")
				        {
					        if (const auto parsed = text::ParseInt(entry.value); parsed && *parsed > 0)
					        {
						        settings.window.width = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "window.height")
				        {
					        if (const auto parsed = text::ParseInt(entry.value); parsed && *parsed > 0)
					        {
						        settings.window.height = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "graphics.vsync" || entry.fullKey == "vsync")
				        {
					        if (const auto parsed = text::ParseBool(entry.value))
					        {
						        settings.graphics.vsync = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "graphics.fxaa")
				        {
					        if (const auto parsed = text::ParseBool(entry.value))
					        {
						        settings.graphics.fxaa = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "graphics.asynccompute")
				        {
					        if (const auto parsed = text::ParseBool(entry.value))
					        {
						        settings.graphics.asyncCompute = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "app.targetfps")
				        {
					        if (const auto parsed = text::ParseFloat(entry.value); parsed && *parsed >= 0.0f)
					        {
						        settings.app.targetFps = *parsed;
					        }
				        }
			        });
		}

		void ParseSettingsStream(std::istream& in, EngineSettings& settings)
		{
			std::stringstream buffer;
			buffer << in.rdbuf();
			ParseSettingsText(buffer.str(), settings);
		}
	} // namespace

	std::filesystem::path EngineSettingsIO::ResolvePath(std::string_view fileName)
	{
		const auto cwd = std::filesystem::current_path();
		const std::filesystem::path requested(fileName);
		if (requested.is_absolute())
		{
			return requested;
		}

		const std::filesystem::path candidates[] = {
			cwd / "data" / "config" / requested,
			cwd / ".." / "data" / "config" / requested,
			cwd / ".." / ".." / "data" / "config" / requested,
			cwd / "config" / requested,
			cwd / ".." / "config" / requested,
			cwd / ".." / ".." / "config" / requested,
			cwd / requested,
			cwd / ".." / requested,
			cwd / ".." / ".." / requested,
		};

		for (const auto& candidate: candidates)
		{
			if (std::filesystem::exists(candidate))
			{
				return candidate;
			}
		}
		return candidates[0];
	}

	void EngineSettingsIO::Save(const EngineSettings& settings, const std::filesystem::path& path)
	{
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		std::ofstream out(path, std::ios::trunc);
		if (!out.is_open())
		{
			WARN(LogCategory::Engine, "Failed to write settings file: {}", path.string());
			return;
		}

		out << "# AetherCore settings (TOML)\n\n";
		out << "[window]\n";
		out << "width = " << settings.window.width << "\n";
		out << "height = " << settings.window.height << "\n\n";
		out << "[graphics]\n";
		out << "vsync = " << (settings.graphics.vsync ? "true" : "false") << "\n";
		out << "fxaa = " << (settings.graphics.fxaa ? "true" : "false") << "\n";
		out << "asyncCompute = " << (settings.graphics.asyncCompute ? "true" : "false") << "\n\n";
		out << "[app]\n";
		out << "# 0 = auto policy (sync to display when VSync on, uncapped when off)\n";
		out << "targetFps = " << settings.app.targetFps << "\n";
	}

	EngineSettings EngineSettingsIO::LoadOrCreate(std::string_view fileName)
	{
		EngineSettings settings;
		const std::string requestedFile(fileName);
		const std::string virtualPath = "config://" + requestedFile;

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
					ParseSettingsText(text, settings);
					INFO(LogCategory::Engine, "Settings loaded from {} ({}x{}, VSync={}, FXAA={}, AsyncCompute={}, TargetFPS={})", virtualPath, settings.window.width, settings.window.height, settings.graphics.vsync ? "on" : "off", settings.graphics.fxaa ? "on" : "off", settings.graphics.asyncCompute ? "on" : "off", settings.app.targetFps);
					return settings;
				}
			}
			catch (const std::exception& e)
			{
				WARN(LogCategory::Engine, "VFS settings read failed ({}): {}", virtualPath, e.what());
			}
		}

		const std::filesystem::path path = ResolvePath(fileName);

		if (!std::filesystem::exists(path))
		{
			INFO(LogCategory::Engine, "Settings file missing; writing defaults to {}", path.string());
			Save(settings, path);
			return settings;
		}

		std::ifstream in(path);
		if (!in.is_open())
		{
			WARN(LogCategory::Engine, "Failed to open settings file: {}. Using defaults.", path.string());
			return settings;
		}

		ParseSettingsStream(in, settings);

		INFO(LogCategory::Engine, "Settings loaded from {} ({}x{}, VSync={}, FXAA={}, AsyncCompute={}, TargetFPS={})", path.string(), settings.window.width, settings.window.height, settings.graphics.vsync ? "on" : "off", settings.graphics.fxaa ? "on" : "off", settings.graphics.asyncCompute ? "on" : "off", settings.app.targetFps);
		return settings;
	}
} // namespace aether
