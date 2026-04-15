#include "Window.hpp"

#include "AetherExceptions.hpp"
#include "Logger.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace aether
{
	Window::Window(const char* title, int width, int height)
	{
		INFO(LogCategory::Window, "Initializing window '{}' ({}x{})", title, width, height);

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

		INFO(LogCategory::Window, "Window created successfully.");
	}

	Window::~Window()
	{
		VERBOSE(LogCategory::Window, "Destroying window resources.");

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
} // namespace aether
