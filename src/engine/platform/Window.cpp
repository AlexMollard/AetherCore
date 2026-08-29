#include "platform/Window.hpp"
#include "platform/WindowPlacement.hpp"

#include <algorithm>

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

	Window::Window(const char* title, int width, int height, const Mode mode, const bool startHidden)
	{
		AE_PROFILE_ZONE();
		AE_INFO(LogCategory::Window, "Initializing window '{}' ({}x{})", title, width, height);

		if (glfwInit() == 0)
		{
			throw WindowError("Failed to initialize GLFW.");
		}

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

		// Borderless must match the monitor's CURRENT video mode exactly, down to the refresh
		// rate hints. A borderless window even one pixel off the output, or on a different
		// mode, silently drops back to composition and gives up the frame it was created to
		// save. Falling back to windowed on a headless/monitor-less system is deliberate:
		// failing to create a window at all would be worse than losing the latency.
		GLFWmonitor* monitor = (mode == Mode::Windowed) ? nullptr : glfwGetPrimaryMonitor();
		const GLFWvidmode* videoMode = (monitor != nullptr) ? glfwGetVideoMode(monitor) : nullptr;
		if (videoMode != nullptr)
		{
			width = videoMode->width;
			height = videoMode->height;
			if (mode == Mode::Borderless)
			{
				glfwWindowHint(GLFW_RED_BITS, videoMode->redBits);
				glfwWindowHint(GLFW_GREEN_BITS, videoMode->greenBits);
				glfwWindowHint(GLFW_BLUE_BITS, videoMode->blueBits);
				glfwWindowHint(GLFW_REFRESH_RATE, videoMode->refreshRate);
				glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
				monitor = nullptr; // borderless is a window that covers the monitor, not an exclusive mode
			}
		}
		else if (mode != Mode::Windowed)
		{
			AE_WARN(LogCategory::Window, "No monitor available; falling back to a windowed presentation.");
			monitor = nullptr;
		}

		if (startHidden)
		{
			glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		}
		m_window = glfwCreateWindow(width, height, title, monitor, nullptr);
		if (m_window == nullptr)
		{
			glfwTerminate();
			throw WindowError("Failed to create GLFW window.");
		}

		if (mode == Mode::Borderless && videoMode != nullptr)
		{
			// Place it on the monitor we sized against, not at the desktop origin - on a
			// multi-monitor desktop those are different points, and being off the output is
			// exactly what disqualifies independent flip.
			int monitorX = 0;
			int monitorY = 0;
			glfwGetMonitorPos(glfwGetPrimaryMonitor(), &monitorX, &monitorY);
			glfwSetWindowPos(m_window, monitorX, monitorY);
		}

		m_mode = mode;

		// A windowed window is created at exactly the configured size, with no reference to
		// the display it lands on. The shipped default is 2560x1440, so a published game on
		// a 1080p screen - or any laptop - opened a window bigger than the desktop, with the
		// title bar off the top and no way to drag or resize it back. Borderless and
		// fullscreen are already sized from the video mode above and must not be touched.
		//
		// Only when it does NOT fit. Repositioning a window that was already fine posts a
		// move (and on a mixed-DPI desktop a scale change) that lands as a framebuffer
		// resize on the first frame, and the first present then returns OUT_OF_DATE - a
		// recreate the engine handles, but noise it should not be creating for itself.
		if (mode == Mode::Windowed)
		{
			FitToPrimaryWorkArea();
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

	void Window::FitToPrimaryWorkArea()
	{
		if (m_window == nullptr || m_mode != Mode::Windowed)
		{
			return;
		}
		GLFWmonitor* primary = glfwGetPrimaryMonitor();
		if (primary == nullptr)
		{
			return; // headless: nothing to fit into
		}

		int width = 0;
		int height = 0;
		glfwGetWindowSize(m_window, &width, &height);
		int frameLeft = 0;
		int frameTop = 0;
		int frameRight = 0;
		int frameBottom = 0;
		glfwGetWindowFrameSize(m_window, &frameLeft, &frameTop, &frameRight, &frameBottom);

		int areaX = 0;
		int areaY = 0;
		int areaW = 0;
		int areaH = 0;
		glfwGetMonitorWorkarea(primary, &areaX, &areaY, &areaW, &areaH);

		const WindowFit fit = FitWindowToWorkArea(width, height, frameLeft + frameRight, frameTop + frameBottom, areaX, areaY, areaW, areaH, areaX + areaW / 2, areaY + areaH / 2);
		if (!fit.resized)
		{
			// It already fits. Leave the placement the platform chose - moving it here buys
			// nothing and costs a spurious resize on the first frame.
			return;
		}
		glfwSetWindowSize(m_window, fit.clientWidth, fit.clientHeight);
		glfwSetWindowPos(m_window, fit.outerX + frameLeft, fit.outerY + frameTop);
		AE_INFO(LogCategory::Window, "Window {}x{} (+{}x{} chrome) does not fit the {}x{} work area; opened at {}x{}.", width, height, frameLeft + frameRight, frameTop + frameBottom, areaW, areaH, fit.clientWidth,
		        fit.clientHeight);
	}

	void Window::GetPrimaryWorkAreaCenter(int& outX, int& outY)
	{
		outX = 0;
		outY = 0;
		GLFWmonitor* primary = glfwGetPrimaryMonitor();
		if (primary == nullptr)
		{
			return; // headless: leave it at the origin and let the clamp be a no-op
		}
		int areaX = 0;
		int areaY = 0;
		int areaW = 0;
		int areaH = 0;
		glfwGetMonitorWorkarea(primary, &areaX, &areaY, &areaW, &areaH);
		outX = areaX + areaW / 2;
		outY = areaY + areaH / 2;
	}

	void Window::GetDesktopCenter(int& outX, int& outY) const
	{
		outX = 0;
		outY = 0;
		if (m_window == nullptr)
		{
			return;
		}
		int posX = 0;
		int posY = 0;
		int width = 0;
		int height = 0;
		glfwGetWindowPos(m_window, &posX, &posY);
		glfwGetWindowSize(m_window, &width, &height);
		outX = posX + width / 2;
		outY = posY + height / 2;
	}

	void Window::CenterOnDesktopPoint(const int x, const int y)
	{
		// Only a windowed window has a position to choose. Borderless and fullscreen are
		// pinned to a monitor by definition, and moving them is what disqualifies the
		// independent-flip path they exist for.
		if (m_window == nullptr || m_mode != Mode::Windowed)
		{
			return;
		}

		int width = 0;
		int height = 0;
		glfwGetWindowSize(m_window, &width, &height);
		if (width <= 0 || height <= 0)
		{
			return;
		}

		// Everything below works in OUTER size. glfwGetWindowSize reports the client area, so
		// sizing against it silently ignores the title bar and borders - the window then looks
		// like it fits while its bottom edge, and the status bar with it, sits off-screen.
		int frameLeft = 0;
		int frameTop = 0;
		int frameRight = 0;
		int frameBottom = 0;
		glfwGetWindowFrameSize(m_window, &frameLeft, &frameTop, &frameRight, &frameBottom);
		const int chromeW = frameLeft + frameRight;
		const int chromeH = frameTop + frameBottom;

		// The work area of the monitor holding the requested point, so the window lands on the
		// same screen the caller was on and never under the taskbar.
		int monitorCount = 0;
		GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
		int areaX = 0;
		int areaY = 0;
		int areaW = 0;
		int areaH = 0;
		bool found = false;
		for (int i = 0; i < monitorCount && !found; ++i)
		{
			int mx = 0;
			int my = 0;
			int mw = 0;
			int mh = 0;
			glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
			if (x >= mx && x < mx + mw && y >= my && y < my + mh)
			{
				areaX = mx;
				areaY = my;
				areaW = mw;
				areaH = mh;
				found = true;
			}
		}
		if (!found)
		{
			if (GLFWmonitor* primary = glfwGetPrimaryMonitor(); primary != nullptr)
			{
				glfwGetMonitorWorkarea(primary, &areaX, &areaY, &areaW, &areaH);
			}
			else
			{
				return;
			}
		}

		// Shrink to fit before placing. A configured size larger than the usable work area -
		// easy to hit on a scaled display, and the default on any screen smaller than the
		// shipped 2560x1440 - puts the bottom and right of the window off-screen, taking the
		// title bar with it. Nothing can be centred in a space it does not fit in.
		const WindowFit fit = FitWindowToWorkArea(width, height, chromeW, chromeH, areaX, areaY, areaW, areaH, x, y);
		if (fit.resized)
		{
			glfwSetWindowSize(m_window, fit.clientWidth, fit.clientHeight);
			AE_INFO(LogCategory::Window, "Window {}x{} (+{}x{} chrome) exceeds the {}x{} work area; fitted to {}x{}.", width, height, chromeW, chromeH, areaW, areaH, fit.clientWidth, fit.clientHeight);
		}

		// glfwSetWindowPos places the CLIENT area, so offset by the frame to keep the whole
		// window - title bar included - inside the work area.
		glfwSetWindowPos(m_window, fit.outerX + frameLeft, fit.outerY + frameTop);
	}

	void Window::Show()
	{
		if (m_window != nullptr)
		{
			glfwShowWindow(m_window);
		}
	}

	void Window::RequestClose()
	{
		if (m_window != nullptr)
		{
			glfwSetWindowShouldClose(m_window, GLFW_TRUE);
		}
	}

	void Window::CancelClose()
	{
		if (m_window != nullptr)
		{
			glfwSetWindowShouldClose(m_window, GLFW_FALSE);
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

	void Window::SetMode(Mode mode)
	{
		if (m_window == nullptr || mode == m_mode)
		{
			return;
		}

		// Capture the rectangle to come back to BEFORE leaving windowed. Once GLFW has
		// resized the window to the video mode there is nothing left to remember.
		if (m_mode == Mode::Windowed)
		{
			glfwGetWindowPos(m_window, &m_windowedX, &m_windowedY);
			glfwGetWindowSize(m_window, &m_windowedWidth, &m_windowedHeight);
		}

		GLFWmonitor* const monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode* const videoMode = (monitor != nullptr) ? glfwGetVideoMode(monitor) : nullptr;
		if (mode != Mode::Windowed && videoMode == nullptr)
		{
			// Same choice the constructor makes: staying in the mode that works beats
			// covering a monitor that is not there.
			AE_WARN(LogCategory::Window, "No monitor available; leaving the window mode unchanged.");
			return;
		}

		switch (mode)
		{
			case Mode::Windowed:
			{
				// A window that has only ever been borderless or fullscreen has no remembered
				// rectangle, so fall back to a reasonable fraction of the display rather than
				// to zero.
				if (m_windowedWidth < 1 || m_windowedHeight < 1)
				{
					m_windowedWidth = (videoMode != nullptr) ? (videoMode->width * 3 / 4) : 1280;
					m_windowedHeight = (videoMode != nullptr) ? (videoMode->height * 3 / 4) : 720;
					m_windowedX = 0;
					m_windowedY = 0;
				}
				glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
				glfwSetWindowMonitor(m_window, nullptr, m_windowedX, m_windowedY, m_windowedWidth, m_windowedHeight, GLFW_DONT_CARE);
				m_mode = mode;
				// The remembered rectangle came from a different mode's desktop; make sure it
				// still fits the work area it is being restored into.
				FitToPrimaryWorkArea();
				break;
			}
			case Mode::Borderless:
			{
				// Undecorate FIRST: changing the decoration of a positioned window moves it by
				// the frame size, so doing it after placement would leave the window off the
				// output by exactly the amount that disqualifies independent flip.
				glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
				int monitorX = 0;
				int monitorY = 0;
				glfwGetMonitorPos(monitor, &monitorX, &monitorY);
				// Monitor stays null: borderless is a window that covers the output, not an
				// exclusive mode.
				glfwSetWindowMonitor(m_window, nullptr, monitorX, monitorY, videoMode->width, videoMode->height, GLFW_DONT_CARE);
				m_mode = mode;
				break;
			}
			case Mode::Fullscreen:
			{
				glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
				glfwSetWindowMonitor(m_window, monitor, 0, 0, videoMode->width, videoMode->height, videoMode->refreshRate);
				m_mode = mode;
				break;
			}
		}

		// GLFW posts the framebuffer-size callback for these changes on most platforms, but
		// not for every transition - notably one where the pixel dimensions happen to match.
		// Flagging it unconditionally costs one redundant swapchain recreate at worst, and
		// guarantees the renderer is never left presenting to a surface of the old shape.
		m_framebufferResized.store(true, std::memory_order_release);

		AE_INFO(LogCategory::Window, "Window mode changed to {}.", mode == Mode::Windowed ? "windowed" : (mode == Mode::Borderless ? "borderless" : "fullscreen"));
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
