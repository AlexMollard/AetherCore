#include "platform/Window.hpp"

#include <cmath>

#include "utils/AetherExceptions.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#endif

namespace aether
{
	void Window::EnableHighDpiAwareness()
	{
#ifdef _WIN32
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
	}

	Window::Window(const char* title, int width, int height)
	{
		AE_PROFILE_ZONE();
		AE_INFO(LogCategory::Window, "Initializing window '{}' ({}x{})", title, width, height);

		if (glfwInit() == 0)
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

		glfwSetWindowUserPointer(m_window, this);
		glfwSetFramebufferSizeCallback(m_window, &Window::FramebufferSizeCallback);

		float xScale = 1.0f;
		float yScale = 1.0f;
		glfwGetWindowContentScale(m_window, &xScale, &yScale);
		int fbW = 0;
		int fbH = 0;
		glfwGetFramebufferSize(m_window, &fbW, &fbH);
		AE_INFO(LogCategory::Window, "Window created: {}x{} framebuffer, content scale {:.2f}x.", fbW, fbH, xScale);
	}

	void Window::FramebufferSizeCallback(GLFWwindow* window, int /*width*/, int /*height*/)
	{
		if (auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window)))
		{
			self->m_framebufferResized.store(true, std::memory_order_release);
		}
	}

	Window::~Window()
	{
		AE_PROFILE_ZONE();
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
		return glfwWindowShouldClose(m_window) != 0;
	}

	void Window::RequestClose()
	{
		if (m_window != nullptr)
		{
			glfwSetWindowShouldClose(m_window, GLFW_TRUE);
		}
	}

	void Window::PollEvents()
	{
		AE_PROFILE_ZONE();
		glfwPollEvents();
	}

	FramebufferSize Window::GetFramebufferSize() const
	{
		int w = 0;
		int h = 0;
		glfwGetFramebufferSize(m_window, &w, &h);
		return {.width = w, .height = h};
	}

	FramebufferSize Window::WaitForValidFramebufferSize()
	{
		AE_PROFILE_ZONE();
		auto size = GetFramebufferSize();
		while (size.width == 0 || size.height == 0)
		{
			glfwWaitEvents();
			size = GetFramebufferSize();
		}
		return size;
	}

	void Window::SetSize(int width, int height)
	{
		if (m_window == nullptr || width < 1 || height < 1)
		{
			return;
		}
		glfwSetWindowSize(m_window, width, height);
	}

	void Window::SetMinimumSize(int minWidth, int minHeight)
	{
		if (m_window == nullptr || minWidth < 1 || minHeight < 1)
		{
			return;
		}
		glfwSetWindowSizeLimits(m_window, minWidth, minHeight, GLFW_DONT_CARE, GLFW_DONT_CARE);
	}

	FramebufferSize Window::GetWindowSize() const
	{
		int w = 0;
		int h = 0;
		if (m_window != nullptr)
		{
			glfwGetWindowSize(m_window, &w, &h);
		}
		return {.width = w, .height = h};
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
