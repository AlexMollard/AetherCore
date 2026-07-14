#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace aether
{
	struct EngineSettings
	{
		struct Window
		{
			int width = 2560;
			int height = 1440;
		} window;

		struct Graphics
		{
			bool vsync = true;
			bool fxaa = false;
			bool asyncCompute = true;
			bool imguiViewports = true;
			float uiScale = 1.0f;
		} graphics;

		struct App
		{
			float targetFps = 0.0f;
			std::string startupScene;
			bool autoplay = false;
		} app;
	};

	[[nodiscard]] inline std::pair<std::string_view, std::string_view> SplitSettingKey(std::string_view key)
	{
		const auto dot = key.find('.');
		if (dot == std::string_view::npos)
		{
			return {std::string_view{}, key};
		}
		return {key.substr(0, dot), key.substr(dot + 1)};
	}

	// from this list, so they can never drift out of sync. Keep entries grouped by
	template<class S, class F>
	void ForEachSettingField(S& settings, F&& f)
	{
		f("window.width", settings.window.width);
		f("window.height", settings.window.height);
		f("graphics.vsync", settings.graphics.vsync);
		f("graphics.fxaa", settings.graphics.fxaa);
		f("graphics.asyncCompute", settings.graphics.asyncCompute);
		f("graphics.imguiViewports", settings.graphics.imguiViewports);
		f("graphics.uiScale", settings.graphics.uiScale);
		f("app.targetFps", settings.app.targetFps);
		f("app.startupScene", settings.app.startupScene);
		f("app.autoplay", settings.app.autoplay);
	}

	struct LoadedEngineSettings
	{
		EngineSettings values;
		EngineSettings base;
	};

	// the base (1+2+3), so keys the user never touched keep tracking shipped/project
	class EngineSettingsIO
	{
	public:
		[[nodiscard]] static LoadedEngineSettings LoadLayered(std::string_view shippedFile = "EngineSettings.toml", const std::filesystem::path& projectFile = {}, std::string_view userFile = "UserSettings.toml");

		[[nodiscard]] static EngineSettings LoadOrCreate(std::string_view shippedFile = "EngineSettings.toml", const std::filesystem::path& projectFile = {}, std::string_view userFile = "UserSettings.toml");

		// settings file (io::PlatformPaths::GetUserConfigDir()/userFile). Never
		static void SaveUserOverrides(const EngineSettings& settings, const EngineSettings& base, std::string_view userFile = "UserSettings.toml");

		static void Apply(std::string_view tomlText, EngineSettings& settings);

		// Clamps fields to valid ranges after a merge (e.g. window dimensions must
		static void Sanitize(EngineSettings& settings);

		[[nodiscard]] static std::string Serialize(const EngineSettings& settings);

		[[nodiscard]] static std::string SerializeOverrides(const EngineSettings& settings, const EngineSettings& base);

		// (release layout) over working-directory-relative ones (dev layout).
		[[nodiscard]] static std::filesystem::path ResolvePath(std::string_view fileName = "EngineSettings.toml");
	};
} // namespace aether
