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

	private:
		GLFWwindow* m_window = nullptr;
	};
} // namespace aether
