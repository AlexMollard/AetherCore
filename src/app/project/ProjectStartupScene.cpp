#include "project/ProjectStartupScene.hpp"

#include <sstream>
#include <string>
#include <system_error>

#include "assets/AssetCatalog.hpp"
#include "scene/SceneSerializer.hpp"
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
		return config.GetRawString(kStartupSceneKey);
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

		// [publish] was build-machine configuration - including an absolute output path - in
		// a file shared through git. Publish derives all of it now, so drop the dead keys the
		// first time we write the file rather than leaving them to confuse the next reader.
		for (const std::string_view retired: {"publish.productname",
		                                      "publish.platformname",
		                                      "publish.outputroot",
		                                      "publish.cleanoutput",
		                                      "publish.buildscripts",
		                                      "publish.usepackagetemplate",
		                                      "publish.verifyoutput",
		                                      "publish.synceditorpak",
		                                      "publish.openafter"})
		{
			config.Erase(retired);
		}

		std::ostringstream buffer;
		config.Save(buffer, "AetherCore project file.");
		if (auto result = io::file_util::WriteText(projectFile, buffer.str()); !result)
		{
			error = "Could not write project settings: " + result.error().message;
			return false;
		}
		return true;
	}

	std::optional<std::filesystem::path> ProjectScenePath(const std::filesystem::path& scenesDir, const std::string_view sceneName, const std::filesystem::path& projectRoot)
	{
		if (sceneName.empty()) return std::nullopt;
		std::filesystem::path path;
		if (sceneName.starts_with("uid://") || sceneName.starts_with("project://"))
		{
			if (projectRoot.empty()) return std::nullopt;
			std::string reference(sceneName);
			if (sceneName.starts_with("uid://"))
			{
				const auto catalog = AssetCatalog::Load(projectRoot);
				if (!catalog) return std::nullopt;
				reference = catalog->Resolve(reference);
			}
			if (!reference.starts_with("project://") || !reference.ends_with(kSceneSuffix)) return std::nullopt;
			const auto relative = std::filesystem::u8path(reference.substr(10)).lexically_normal();
			if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return std::nullopt;
			path = projectRoot / relative;
		}
		else
		{
			if (scenesDir.empty() || !scene::IsValidAssetName(sceneName)) return std::nullopt;
			path = scenesDir / std::filesystem::u8path(std::string(sceneName) + std::string(kSceneSuffix));
		}
		return path;
	}

	bool ProjectHasScene(const std::filesystem::path& scenesDir, const std::string_view sceneName, const std::filesystem::path& projectRoot)
	{
		const auto path = ProjectScenePath(scenesDir, sceneName, projectRoot);
		std::error_code ec;
		return path && std::filesystem::is_regular_file(*path, ec);
	}

	bool ValidateProjectStartupScene(const std::filesystem::path& scenesDir, const std::string_view sceneName, std::string& error, const std::filesystem::path& projectRoot)
	{
		if (sceneName.empty())
		{
			error = "No startup scene is set. Pick one in the Project panel (or the star in the Scenes list) so the game knows what to boot.";
			return false;
		}
		if (!ProjectHasScene(scenesDir, sceneName, projectRoot))
		{
			error = "Startup scene '" + std::string(sceneName) + "' does not exist in this project.";
			return false;
		}
		return true;
	}
} // namespace aether::app
