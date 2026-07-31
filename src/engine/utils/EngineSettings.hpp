#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace aether
{
	// Applied when MAILBOX is requested with no frame cap, which would otherwise run the loop
	// flat out. Deliberately above 60 so the extra headroom still buys latency, and low
	// enough that it is not a thermal event.
	inline constexpr float kUncappedMailboxFallbackFps = 120.0f;

	struct EngineSettings
	{
		struct Window
		{
			int width = 2560;
			int height = 1440;
			// "windowed" | "borderless" | "fullscreen". Borderless is the one worth reaching
			// for: it is the only mode besides exclusive fullscreen that can win DWM
			// independent flip, and composition costs about a frame of latency. Unparsable
			// values fall back to windowed rather than guessing.
			std::string mode = "windowed";
		} window;

		struct Graphics
		{
			bool vsync = true;
			// How many frames the producer may run ahead of the screen. This is a LATENCY
			// control, not an allocation one: per-frame resources stay sized at
			// Swapchain::kMaxFramesInFlight, and this only throttles the game thread sooner.
			// Every frame of run-ahead is a vsync interval of input lag (~16.7 ms at 60 Hz),
			// so 3 costs ~50 ms; 2 trades a little CPU/GPU overlap for a frame of it, and 1
			// is lowest-latency but leaves the GPU idle while the CPU works. Clamped to
			// [1, Swapchain::kMaxFramesInFlight].
			int framesInFlight = 2;
			// With vsync on, prefer MAILBOX over FIFO. Both are tear-free; FIFO makes each
			// present queue behind the last, while MAILBOX replaces the pending image, so a
			// frame reaches the screen without waiting its turn. Costs GPU work on frames
			// that get replaced, and does nothing when vsync is off (that is already
			// IMMEDIATE). Falls back to FIFO wherever the driver lacks MAILBOX.
			bool lowLatencyPresent = false;

			// Render the SCENE at this fraction of the output and let the existing final
			// fullscreen pass upscale it; UI still draws at native resolution on top, so text
			// and sprites stay sharp. This is what makes borderless viable on a 4K panel,
			// where covering the output means 2.25x the pixels of a 1440p window.
			float renderScale = 1.0f;
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

		// The engine draws the mouse pointer itself when a project asks it to (see ui::CursorService),
		// which is how a game gets its own cursor without writing one. Off by default: tools and the
		// editor want the real OS pointer.
		struct Cursor
		{
			bool custom = false;
			std::string texture;       // VFS path to the pointer art; empty draws nothing
			float size = 32.0f;        // on-screen square size in pixels
			float hotspotX = 0.0f;     // 0..1 across the image: the pixel that sits on the mouse
			float hotspotY = 0.0f;
			bool pixelArt = true;      // nearest sampling, so small art scales up crisp
		} cursor;
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
		f("window.mode", settings.window.mode);
		f("graphics.vsync", settings.graphics.vsync);
		f("graphics.framesInFlight", settings.graphics.framesInFlight);
		f("graphics.lowLatencyPresent", settings.graphics.lowLatencyPresent);
		f("graphics.renderScale", settings.graphics.renderScale);
		f("graphics.fxaa", settings.graphics.fxaa);
		f("graphics.asyncCompute", settings.graphics.asyncCompute);
		f("graphics.imguiViewports", settings.graphics.imguiViewports);
		f("graphics.uiScale", settings.graphics.uiScale);
		f("app.targetFps", settings.app.targetFps);
		f("app.startupScene", settings.app.startupScene);
		f("app.autoplay", settings.app.autoplay);
		f("cursor.custom", settings.cursor.custom);
		f("cursor.texture", settings.cursor.texture);
		f("cursor.size", settings.cursor.size);
		f("cursor.hotspotX", settings.cursor.hotspotX);
		f("cursor.hotspotY", settings.cursor.hotspotY);
		f("cursor.pixelArt", settings.cursor.pixelArt);
	}

	// Keys that belong to the project, never to the machine. The startup scene is the one
	// that matters: it decides what a published game boots, and the bake reads the project
	// layer only. Letting it sit in the per-user file gives an editor that boots the right
	// scene on the machine that set it and an empty world everywhere else - including in
	// every published build. So the user layer neither writes these nor reads them back.
	[[nodiscard]] inline bool IsProjectOnlySettingKey(std::string_view key) noexcept
	{
		return key == "app.startupScene";
	}

	// Which keys a document is allowed to contribute. UserOverridable drops the
	// project-only keys above, so a stale machine-local file can never mask the project.
	enum class SettingsScope
	{
		All,
		UserOverridable,
	};

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

		static void Apply(std::string_view tomlText, EngineSettings& settings, SettingsScope scope = SettingsScope::All);

		// Clamps fields to valid ranges after a merge (e.g. window dimensions must
		static void Sanitize(EngineSettings& settings);

		[[nodiscard]] static std::string Serialize(const EngineSettings& settings);

		[[nodiscard]] static std::string SerializeOverrides(const EngineSettings& settings, const EngineSettings& base);

		// (release layout) over working-directory-relative ones (dev layout).
		[[nodiscard]] static std::filesystem::path ResolvePath(std::string_view fileName = "EngineSettings.toml");
	};
} // namespace aether
