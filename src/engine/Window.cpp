#include "Window.hpp"

#include <stdexcept>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace meow
{
	Window::Window(const char* title, int width, int height)
	{
		if (!glfwInit())
		{
			throw std::runtime_error("Failed to initialize GLFW.");
		}

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
		if (m_window == nullptr)
		{
			glfwTerminate();
			throw std::runtime_error("Failed to create GLFW window.");
		}
	}

	Window::~Window()
	{
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
}