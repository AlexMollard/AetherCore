#pragma once

#include <optional>

#include "platform/Input.hpp"
#include "platform/Window.hpp"

namespace aether
{
	// Owns the window and input services. Must be initialized before any
	class PlatformSubsystem
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			int width = 1280;
			int height = 720;
		};

		void Init(const Config& config);
		void Shutdown();

		[[nodiscard]] Window& GetWindow()
		{
			return *m_window;
		}

		[[nodiscard]] Input& GetInput()
		{
			return m_input;
		}

	private:
		std::optional<Window> m_window;
		Input m_input;
	};
} // namespace aether
