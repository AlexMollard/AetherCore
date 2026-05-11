#pragma once

struct GLFWwindow;

namespace aether
{
	class Window
	{
	public:
		Window(const char* title, int width, int height);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		[[nodiscard]] GLFWwindow* GetHandle() const;
		[[nodiscard]] bool ShouldClose() const;
		void PollEvents() const;

		// Returns the refresh rate (Hz) of the monitor the window currently occupies.
		// Falls back to the primary monitor for windowed mode.
		// Returns 0 if the refresh rate cannot be determined.
		[[nodiscard]] int GetDisplayRefreshRate() const;

	private:
		GLFWwindow* m_window = nullptr;
	};
} // namespace aether
