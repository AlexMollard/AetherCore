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
		// or windowing init (i.e. first thing in main). Without it, Windows
		// virtualizes a high-DPI monitor (e.g. a 2560x1440 display at 133% scaling
		// reports 1920x1080 and upscales - blurry, and the swapchain is only 1080p).
		// With it, GLFW reports the true framebuffer + content scale, and ImGui's
		// ConfigDpiScaleFonts/Viewports keep the UI crisp and correctly sized.
		// No-op off Windows. Safe to call once; extra calls are ignored.
		static void EnableHighDpiAwareness();

		Window(const char* title, int width, int height);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		[[nodiscard]] GLFWwindow* GetHandle() const;
		[[nodiscard]] bool ShouldClose() const;
		static void PollEvents();

		[[nodiscard]] FramebufferSize GetFramebufferSize() const;
		FramebufferSize WaitForValidFramebufferSize();

		// Requests a new window size. GLFW fires the framebuffer-size callback on the
		// next PollEvents, which drives the main-thread swapchain recreate. Must be
		// called from the main thread.
		void SetSize(int width, int height);

		[[nodiscard]] int GetDisplayRefreshRate() const;

		// Set by the GLFW framebuffer-size callback (fires on the main thread
		// during PollEvents). Consume() clears the flag; Peek() does not. Used by
		// the main-thread quiesced swapchain recreate so resize is detected on the
		// producer thread rather than raced off the render thread.
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
		std::atomic<bool> m_framebufferResized{false};
	};
} // namespace aether
