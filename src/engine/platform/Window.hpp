#pragma once

struct GLFWwindow;

namespace aether
{
	struct FramebufferSize
	{
		int width = 0;
		int height = 0;
	};

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

		[[nodiscard]] FramebufferSize GetFramebufferSize() const;
		FramebufferSize WaitForValidFramebufferSize();

		[[nodiscard]] int GetDisplayRefreshRate() const;

	private:
		GLFWwindow* m_window = nullptr;
	};
} // namespace aether
