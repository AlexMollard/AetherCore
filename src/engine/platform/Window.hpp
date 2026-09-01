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

		// `startHidden` creates the window without mapping it, to be revealed later with
		// Show(). Used for the launcher handoff: the editor takes several seconds to load a
		// project, and a window that appears immediately spends that time as an empty
		// rectangle sitting on top of the launcher, which is still up because it waits for the
		// editor to report healthy. Two windows, one of them blank, then one vanishes.
		// Staying hidden until ready turns that into a single swap.
		Window(const char* title, int width, int height, Mode mode = Mode::Windowed, bool startHidden = false);
		~Window();

		// Map a window created with startHidden. Safe to call when already visible.
		void Show();

		// Move the window so its centre lands on a desktop point, clamped to stay inside the
		// work area of whichever monitor contains that point.
		//
		// The launcher hands its own centre to the editor so the editor opens where the
		// launcher was. Position rather than size: the editor keeps the resolution the user
		// configured, it just stops appearing somewhere unrelated - which on a multi-monitor
		// desktop could be a different screen entirely, since a windowed GLFW window is placed
		// wherever the OS feels like.
		void CenterOnDesktopPoint(int x, int y);

		// Desktop coordinates of this window's centre. The launcher hands this to the editor.
		void GetDesktopCenter(int& outX, int& outY) const;

		// Centre of the primary monitor's usable area (taskbar excluded). Where a window
		// goes when nothing has asked for somewhere better.
		static void GetPrimaryWorkAreaCenter(int& outX, int& outY);

		// Shrink a windowed window that is larger than the primary display and place it on
		// screen. Does NOTHING when it already fits, so a normal start keeps the platform's
		// own placement and never triggers a resize on the first frame.
		void FitToPrimaryWorkArea();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		[[nodiscard]] GLFWwindow* GetHandle() const;
		[[nodiscard]] bool ShouldClose() const;

		// Whether this window has OS keyboard focus. The cursor position reported by GLFW
		// tracks the mouse across the whole desktop, so without this an editor sitting in the
		// background wakes up and renders flat out because the mouse moved over some other
		// application.
		[[nodiscard]] bool IsFocused() const;
		// Requests a clean application-loop exit on the next close check. Main thread only.
		void RequestClose();
		// Withdraws a close request the OS has already recorded (the title-bar X, Alt+F4).
		// Events are pumped before the layers update and the loop only re-reads the flag on
		// the next iteration, so a layer that clears it here holds the window open - which is
		// what lets the editor ask about unsaved work instead of exiting under the question.
		void CancelClose();
		static void PollEvents();

		// Sleeps on the OS event queue for at most `seconds`, returning the moment anything
		// arrives. This is how an idle editor gives the CPU back without going unresponsive:
		// a plain sleep would hold the first mouse movement for the whole interval, which at
		// a low idle rate is exactly the lag the throttle is supposed to avoid paying for.
		static void WaitEventsTimeout(double seconds);

		// Wakes a thread parked in WaitEventsTimeout. Safe to call from any thread, and the
		// only way a non-input event - a control command, a finished background job - can get
		// an idle editor to run a frame promptly.
		static void PostEmptyEvent();

		[[nodiscard]] FramebufferSize GetFramebufferSize() const;

		// Borderless and fullscreen own their own size; only a windowed window may be resized
		// to a configured width and height.
		[[nodiscard]] Mode GetMode() const
		{
			return m_mode;
		}

		// Change presentation without recreating the window.
		//
		// The windowed rectangle is remembered on the way out and restored on the way back,
		// because borderless and fullscreen overwrite the size with the video mode's. Without
		// that, leaving fullscreen would have to invent a size, and the window a user had
		// carefully placed would come back somewhere else at some other size.
		//
		// Only the presentation changes: this posts a framebuffer resize, which the existing
		// swapchain recreate path already handles, so callers do not have to do anything else.
		// Must be called on the main thread, like every other GLFW call here.
		void SetMode(Mode mode);
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

		// The windowed geometry to return to. Captured whenever the window is windowed and
		// about to stop being so, never while it is already covering a monitor - restoring
		// a monitor-sized rectangle as a "window" is how a decorated window ends up with its
		// title bar off the top of the screen and no way to drag it back.
		int m_windowedX = 0;
		int m_windowedY = 0;
		int m_windowedWidth = 0;
		int m_windowedHeight = 0;

		std::atomic<bool> m_framebufferResized{false};
	};
} // namespace aether
