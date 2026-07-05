#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether
{
	struct EngineSettings
	{
		struct Window
		{
			int width = 1280;
			int height = 720;
		} window;

		struct Graphics
		{
			bool vsync = true;
			bool fxaa = false;
			bool asyncCompute = true;
		} graphics;

		struct App
		{
			// 0 = automatic policy (swapchain-paced when VSync is on, uncapped when off)
			float targetFps = 0.0f;
			// Scene file (resources/scenes/<name>.scene.toml) loaded at boot after
			// the scene script's on_attach; auto-generated from the script content
			// on first run. Empty disables boot-from-scene.
			std::string startupScene = "sandbox";
			// false = boot into the editor's frozen Editing mode (press Play).
			bool autoplay = false;
		} app;
	};

	class EngineSettingsIO
	{
	public:
		// Resolves and loads settings from disk. If the file is missing, defaults are
		// written to disk first and then returned.
		[[nodiscard]] static EngineSettings LoadOrCreate(std::string_view fileName = "engine.toml");

		[[nodiscard]] static std::filesystem::path ResolvePath(std::string_view fileName = "engine.toml");
		static void Save(const EngineSettings& settings, const std::filesystem::path& path);
	};
} // namespace aether
