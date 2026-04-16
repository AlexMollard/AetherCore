#include "EngineSettings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "FileSystem.hpp"
#include "Logger.hpp"

namespace aether
{
	namespace
	{
		std::string TrimAscii(std::string value)
		{
			auto isSpace = [](const unsigned char c) { return std::isspace(c) != 0; };

			while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
				value.erase(value.begin());
			while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
				value.pop_back();
			return value;
		}

		std::string ToLowerAscii(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		std::optional<bool> ParseBool(const std::string& value)
		{
			const std::string lower = ToLowerAscii(value);
			if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
				return true;
			if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
				return false;
			return std::nullopt;
		}

		std::optional<int> ParseInt(const std::string& value)
		{
			try
			{
				return std::stoi(value);
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		std::optional<float> ParseFloat(const std::string& value)
		{
			try
			{
				return std::stof(value);
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		void ParseSettingsStream(std::istream& in, EngineSettings& settings)
		{
			std::string section;
			std::string line;
			while (std::getline(in, line))
			{
				auto hashPos = line.find('#');
				auto semicolonPos = line.find(';');
				const auto commentPos = std::min(hashPos, semicolonPos);
				if (commentPos != std::string::npos)
					line = line.substr(0, commentPos);

				line = TrimAscii(std::move(line));
				if (line.empty())
					continue;

				if (line.front() == '[' && line.back() == ']')
				{
					section = ToLowerAscii(TrimAscii(line.substr(1, line.size() - 2)));
					continue;
				}

				const auto eq = line.find('=');
				if (eq == std::string::npos)
					continue;

				const std::string key = ToLowerAscii(TrimAscii(line.substr(0, eq)));
				const std::string value = TrimAscii(line.substr(eq + 1));
				const std::string full = section.empty() ? key : (section + "." + key);

				if (full == "window.width")
				{
					if (const auto parsed = ParseInt(value); parsed && *parsed > 0)
						settings.window.width = *parsed;
					continue;
				}
				if (full == "window.height")
				{
					if (const auto parsed = ParseInt(value); parsed && *parsed > 0)
						settings.window.height = *parsed;
					continue;
				}
				if (full == "graphics.vsync" || full == "vsync")
				{
					if (const auto parsed = ParseBool(value))
						settings.graphics.vsync = *parsed;
					continue;
				}
				if (full == "graphics.fxaa")
				{
					if (const auto parsed = ParseBool(value))
						settings.graphics.fxaa = *parsed;
					continue;
				}
				if (full == "graphics.asynccompute")
				{
					if (const auto parsed = ParseBool(value))
						settings.graphics.asyncCompute = *parsed;
					continue;
				}
				if (full == "app.targetfps")
				{
					if (const auto parsed = ParseFloat(value); parsed && *parsed >= 0.0f)
						settings.app.targetFps = *parsed;
					continue;
				}
			}
		}
	} // namespace

	std::filesystem::path EngineSettingsIO::ResolvePath(std::string_view fileName)
	{
		const auto cwd = std::filesystem::current_path();
		const std::filesystem::path requested(fileName);
		if (requested.is_absolute())
			return requested;

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
				return candidate;
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

		out << "# AetherCore settings\n";
		out << "# Bool values: true/false, on/off, yes/no, 1/0\n\n";
		out << "[Window]\n";
		out << "Width=" << settings.window.width << "\n";
		out << "Height=" << settings.window.height << "\n\n";
		out << "[Graphics]\n";
		out << "VSync=" << (settings.graphics.vsync ? "true" : "false") << "\n";
		out << "FXAA=" << (settings.graphics.fxaa ? "true" : "false") << "\n";
		out << "AsyncCompute=" << (settings.graphics.asyncCompute ? "true" : "false") << "\n\n";
		out << "[App]\n";
		out << "# 0 = auto policy (sync to display when VSync on, uncapped when off)\n";
		out << "TargetFPS=" << settings.app.targetFps << "\n";
	}

	EngineSettings EngineSettingsIO::LoadOrCreate(std::string_view fileName)
	{
		EngineSettings settings;
		const std::string virtualPath = "config://" + std::string(fileName);
		if (io::FileSystem::IsInitialized())
		{
			try
			{
				if (io::FileSystem::Exists(virtualPath))
				{
					const auto bytes = io::FileSystem::ReadFile(virtualPath);
					std::string text;
					text.resize(bytes.size());
					for (std::size_t i = 0; i < bytes.size(); ++i)
						text[i] = static_cast<char>(bytes[i]);

					std::istringstream in(text);
					ParseSettingsStream(in, settings);
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
