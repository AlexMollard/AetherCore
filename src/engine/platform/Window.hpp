#pragma once

#include <atomic>

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
		// Declare the process per-monitor DPI-aware. MUST be called before any GLFW
		static void EnableHighDpiAwareness();

		// How the window presents. This is a LATENCY decision as much as a cosmetic one: a
		// decorated desktop window can never qualify for DWM independent flip, because that
		// requires the presented surface to cover the output exactly. Composition costs a
		// frame - the whole reason games ship this as a setting rather than hard-coding it.
		enum class Mode
		{
			Windowed,   // decorated, composited by the desktop
			Borderless, // undecorated and exactly covering the monitor; eligible for independent flip
			Fullscreen, // exclusive; bypasses the compositor outright
		};

		Window(const char* title, int width, int height, Mode mode = Mode::Windowed);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		[[nodiscard]] GLFWwindow* GetHandle() const;
		[[nodiscard]] bool ShouldClose() const;
		// Requests a clean application-loop exit on the next close check. Main thread only.
		void RequestClose();
		static void PollEvents();

		[[nodiscard]] FramebufferSize GetFramebufferSize() const;

		// Borderless and fullscreen own their own size; only a windowed window may be resized
		// to a configured width and height.
		[[nodiscard]] Mode GetMode() const
		{
			return m_mode;
		}
		FramebufferSize WaitForValidFramebufferSize();

		[[nodiscard]] FramebufferSize GetWindowSize() const;

		// next PollEvents, which drives the main-thread swapchain recreate. Must be
		void SetSize(int width, int height);

		// maximum stays unbounded. For tool front-ends whose layout has a smallest
		void SetMinimumSize(int minWidth, int minHeight);

		[[nodiscard]] int GetDisplayRefreshRate() const;

		// Set by the GLFW framebuffer-size callback (fires on the main thread
		[[nodiscard]] bool PeekFramebufferResized() const
		{
			return m_framebufferResized.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool ConsumeFramebufferResized()
		{
			return m_framebufferResized.exchange(false, std::memory_order_acq_rel);
		}

	private:
		static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);

		GLFWwindow* m_window = nullptr;

		Mode m_mode = Mode::Windowed;
		std::atomic<bool> m_framebufferResized{false};
	};
} // namespace aether
