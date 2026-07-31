#include "project/ProjectStartupScene.hpp"

#include <sstream>
#include <string>
#include <system_error>

#include "io/FileUtil.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	namespace
	{
		constexpr std::string_view kStartupSceneKey = "app.startupScene";
		constexpr std::string_view kSceneSuffix = ".scene.toml";
	} // namespace

	std::string ReadProjectStartupScene(const std::filesystem::path& projectFile)
	{
		if (projectFile.empty())
		{
			return {};
		}
		auto text = io::file_util::ReadText(projectFile);
		if (!text)
		{
			return {};
		}
		TomlConfig config;
		if (!config.Load(*text))
		{
			return {};
		}
		return config.GetString(kStartupSceneKey);
	}

	bool WriteProjectStartupScene(const std::filesystem::path& projectFile, const std::string_view sceneName, std::string& error)
	{
		if (projectFile.empty())
		{
			error = "No project is open.";
			return false;
		}

		TomlConfig config;
		{
			auto text = io::file_util::ReadText(projectFile);
			std::error_code ec;
			if (!text && std::filesystem::exists(projectFile, ec))
			{
				// The file is there but unreadable. Writing anyway would replace the whole
				// project file with nothing but the key below, destroying its paths,
				// graphics and project sections. Refuse instead.
				error = "Could not read project settings; refusing to overwrite " + projectFile.generic_string();
				return false;
			}
			// A file that does not parse leaves the config empty, so saving would write back
			// nothing but the key below and destroy the rest of the project. Refuse.
			if (text && !config.Load(*text))
			{
				error = "Could not parse project settings; refusing to overwrite " + projectFile.generic_string();
				return false;
			}
		}

		config.Set(kStartupSceneKey, sceneName);

		std::ostringstream buffer;
		config.Save(buffer, "AetherCore project file.");
		if (auto result = io::file_util::WriteText(projectFile, buffer.str()); !result)
		{
			error = "Could not write project settings: " + result.error().message;
			return false;
		}
		return true;
	}

	bool ProjectHasScene(const std::filesystem::path& scenesDir, const std::string_view sceneName)
	{
		if (scenesDir.empty() || sceneName.empty())
		{
			return false;
		}
		std::error_code ec;
		return std::filesystem::exists(scenesDir / (std::string(sceneName) + std::string(kSceneSuffix)), ec);
	}

	bool ValidateProjectStartupScene(const std::filesystem::path& scenesDir, const std::string_view sceneName, std::string& error)
	{
		if (sceneName.empty())
		{
			error = "No startup scene is set. Pick one in the Project panel (or the star in the Scenes list) so the game knows what to boot.";
			return false;
		}
		if (!ProjectHasScene(scenesDir, sceneName))
		{
			error = "Startup scene '" + std::string(sceneName) + "' does not exist in this project (expected " + (scenesDir / (std::string(sceneName) + std::string(kSceneSuffix))).generic_string() + ").";
			return false;
		}
		return true;
	}
} // namespace aether::app
