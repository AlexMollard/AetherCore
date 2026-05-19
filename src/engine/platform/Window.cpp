#include "platform/Window.hpp"

#include "utils/AetherExceptions.hpp"
#include "utils/Logger.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace aether
{
	Window::Window(const char* title, int width, int height)
	{
		AE_INFO(LogCategory::Window, "Initializing window '{}' ({}x{})", title, width, height);

		if (!glfwInit())
		{
			throw WindowError("Failed to initialize GLFW.");
		}

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
		if (m_window == nullptr)
		{
			glfwTerminate();
			throw WindowError("Failed to create GLFW window.");
		}

		AE_INFO(LogCategory::Window, "Window created successfully.");
	}

	Window::~Window()
	{
		AE_VERBOSE(LogCategory::Window, "Destroying window resources.");

		if (m_window != nullptr)
		{
			glfwDestroyWindow(m_window);
		}

		glfwTerminate();
	}

	GLFWwindow* Window::GetHandle() const
	{
		return m_window;
	}

	bool Window::ShouldClose() const
	{
		return glfwWindowShouldClose(m_window);
	}

	void Window::PollEvents() const
	{
		glfwPollEvents();
	}

	FramebufferSize Window::GetFramebufferSize() const
	{
		int w = 0;
		int h = 0;
		glfwGetFramebufferSize(m_window, &w, &h);
		return { w, h };
	}

	FramebufferSize Window::WaitForValidFramebufferSize()
	{
		auto size = GetFramebufferSize();
		while (size.width == 0 || size.height == 0)
		{
			glfwWaitEvents();
			size = GetFramebufferSize();
		}
		return size;
	}

	int Window::GetDisplayRefreshRate() const
	{
		GLFWmonitor* monitor = glfwGetWindowMonitor(m_window);
		if (monitor == nullptr)
		{
			monitor = glfwGetPrimaryMonitor();
		}
		if (monitor == nullptr)
		{
			return 0;
		}
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		return (mode != nullptr) ? mode->refreshRate : 0;
	}
} // namespace aether
