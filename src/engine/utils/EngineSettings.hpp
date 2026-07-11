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
			int width = 2560;  // QHD (1440p) default
			int height = 1440;
		} window;

		struct Graphics
		{
			bool vsync = true;
			bool fxaa = false;
			bool asyncCompute = true;
			bool imguiViewports = true; // tool panels tear out into OS windows
			float uiScale = 1.0f;       // manual editor UI scale multiplier, on top of per-monitor DPI
		} graphics;

		struct App
		{
			// 0 = automatic policy (swapchain-paced when VSync is on, uncapped when off)
			float targetFps = 0.0f;
			// Project scene file loaded at boot. Empty disables boot-from-scene.
			std::string startupScene;
			// false = boot into the editor's frozen Editing mode (press Play).
			bool autoplay = false;
		} app;
	};

	// Splits a dotted setting key ("graphics.fxaa") into ("graphics", "fxaa").
	// A key with no dot yields an empty section and the whole key as the name.
	[[nodiscard]] inline std::pair<std::string_view, std::string_view> SplitSettingKey(std::string_view key)
	{
		const auto dot = key.find('.');
		if (dot == std::string_view::npos)
		{
			return {std::string_view{}, key};
		}
		return {key.substr(0, dot), key.substr(dot + 1)};
	}

	// THE single source of truth mapping each setting to its dotted TOML key.
	// Visits every field as (key, member-reference). Templated on S so one
	// definition binds both EngineSettings& (loading writes fields) and
	// const EngineSettings& (saving/diffing/UI read them).
	//
	// Adding a setting is exactly two lines: the struct field above, and one
	// f(...) line here. Load, save, delta-diff, and the ImGui editor all derive
	// from this list, so they can never drift out of sync. Keep entries grouped by
	// section (window, graphics, app) so serialized output stays tidy.
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

	// Result of a layered load. 'base' is layers 1+2+3 (compiled defaults overlaid
	// with the shipped file, then the project file) with NO user overrides applied,
	// so a caller can diff against it to persist only what the user changed.
	struct LoadedEngineSettings
	{
		EngineSettings values; // merged: defaults -> shipped -> project -> user
		EngineSettings base;   // defaults -> shipped -> project
	};

	// Four-layer settings I/O:
	//   1. compiled-in EngineSettings defaults (the struct above)
	//   2. shipped engine defaults  -> EngineSettings.toml, read-only, resolved
	//      beside the executable (release) or in the build tree (dev)
	//   3. per-project overrides    -> ProjectSettings.toml, an already-resolved
	//      absolute path supplied by the caller; optional
	//   4. per-user overrides       -> UserSettings.toml in the OS user-config dir,
	//      the only file ever written back
	//
	// Loading merges 1 -> 2 -> 3 -> 4. Saving writes ONLY the keys that differ from
	// the base (1+2+3), so keys the user never touched keep tracking shipped/project
	// defaults across updates, and neither the shipped nor project files are ever
	// modified.
	class EngineSettingsIO
	{
	public:
		// Loads and merges all layers, returning both the merged values and the
		// base (1+2+3) for later delta saves. Unlike shippedFile/userFile (looked up
		// via ResolvePath / GetUserConfigDir), projectFile is an already-resolved
		// absolute path supplied by the caller; an empty path skips the project layer.
		[[nodiscard]] static LoadedEngineSettings LoadLayered(std::string_view shippedFile = "EngineSettings.toml",
		                                                      const std::filesystem::path& projectFile = {},
		                                                      std::string_view userFile = "UserSettings.toml");

		// Convenience wrapper returning only the merged values. Kept for callers
		// that don't need to save (e.g. one-shot engine embedders).
		[[nodiscard]] static EngineSettings LoadOrCreate(std::string_view shippedFile = "EngineSettings.toml",
		                                                 const std::filesystem::path& projectFile = {},
		                                                 std::string_view userFile = "UserSettings.toml");

		// Writes only the keys where 'settings' differs from 'base' to the per-user
		// settings file (io::PlatformPaths::GetUserConfigDir()/userFile). Never
		// touches the shipped file.
		static void SaveUserOverrides(const EngineSettings& settings, const EngineSettings& base, std::string_view userFile = "UserSettings.toml");

		// Overlays a TOML document onto 'settings' in place: only keys present in
		// the text are changed. Exposed for layered loading and unit testing.
		static void Apply(std::string_view tomlText, EngineSettings& settings);

		// Clamps fields to valid ranges after a merge (e.g. window dimensions must
		// be >= 1, targetFps >= 0). The single home for per-field domain bounds so
		// the load/save machinery stays purely mechanical.
		static void Sanitize(EngineSettings& settings);

		// Serializes the full settings as a TOML document.
		[[nodiscard]] static std::string Serialize(const EngineSettings& settings);

		// Serializes only the keys where 'settings' differs from 'base'. Returns a
		// header-only document (no key lines) when nothing differs.
		[[nodiscard]] static std::string SerializeOverrides(const EngineSettings& settings, const EngineSettings& base);

		// Resolves a shipped config file, preferring executable-relative locations
		// (release layout) over working-directory-relative ones (dev layout).
		[[nodiscard]] static std::filesystem::path ResolvePath(std::string_view fileName = "EngineSettings.toml");
	};
} // namespace aether
