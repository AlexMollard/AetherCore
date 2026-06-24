#pragma once

#include <filesystem>
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
