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
		Window(const char* title, int width, int height);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		[[nodiscard]] GLFWwindow* GetHandle() const;
		[[nodiscard]] bool ShouldClose() const;
		static void PollEvents();

		[[nodiscard]] FramebufferSize GetFramebufferSize() const;
		FramebufferSize WaitForValidFramebufferSize();

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
