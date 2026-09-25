#pragma once

#include <climits>

#include <cstdint>
#include <functional>
#include <memory>

#include "Defines.hpp" // AE_VALIDATION_DEFAULT_ON (force-included, named here for clarity)
#include "EngineClient.hpp"
#include "ui/CursorService.hpp"
#include "IEngineRuntime.hpp"
#include "RuntimeProfile.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include <atomic>

#include "platform/Window.hpp"
#include "rendering/IUiOverlay.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/CustomPassRegistry.hpp"
#include "rendering/Light2DSubmission.hpp"
#include "rendering/RenderThread.hpp"
#include "rendering/ScreenshotService.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/FramePacer.hpp"
#include "utils/FrameTimeline.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether
{
	struct FrameConstants;

	// The one place settings turn into a present mode, so the three-way choice can never
	// disagree with itself across the call sites that need it.
	[[nodiscard]] constexpr gpu::PresentMode DesiredPresentMode(const EngineSettings& settings) noexcept
	{
		if (!settings.graphics.vsync)
		{
			return gpu::PresentMode::Immediate;
		}
		// MAILBOX never blocks the producer, so with no frame cap the loop renders as fast as
		// it possibly can and discards nearly all of it - measured at 515 fps to present 60,
		// which is most of a GPU spent on frames nobody sees. Uncapped is a legitimate thing
		// to ask for, so the mode stands down instead of the cap being silently rewritten.
		const bool capped = settings.app.targetFps > 0.0f;
		return settings.graphics.lowLatencyPresent && capped ? gpu::PresentMode::Mailbox : gpu::PresentMode::Fifo;
	}

	// Unknown text falls back to Windowed. A typo must not silently hand someone an
	// undecorated window covering their screen with no obvious way back.
	[[nodiscard]] inline Window::Mode ParseWindowMode(const std::string_view mode) noexcept
	{
		if (mode == "borderless")
		{
			return Window::Mode::Borderless;
		}
		if (mode == "fullscreen")
		{
			return Window::Mode::Fullscreen;
		}
		return Window::Mode::Windowed;
	}

	// The inverse, kept next to the parse so the two spellings cannot drift apart. These
	// are the strings in kWindowModes, which is what a settings file holds and what the
	// settings UI offers.
	[[nodiscard]] inline std::string_view WindowModeToString(const Window::Mode mode) noexcept
	{
		switch (mode)
		{
			case Window::Mode::Borderless:
				return "borderless";
			case Window::Mode::Fullscreen:
				return "fullscreen";
			case Window::Mode::Windowed:
				break;
		}
		return "windowed";
	}

	class GpuDevice;
	class CameraSubsystem;
	class RenderingSubsystem;

	namespace audio
	{
		class AudioSubsystem;
	}

	// Owns the whole frame lifecycle: the render thread, the producer/game-thread
	class AetherCore : public IEngineRuntime
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			int width = 1280;
			int height = 720;
			gpu::PresentMode presentMode = gpu::PresentMode::Fifo;
			const char* settingsFile = "EngineSettings.toml";
			// GameRuntime must leave this false: Aftermath is a dev tool (dumps
			bool enableGpuDiagnostics = false;
			// Enable the Vulkan validation layer (dev builds only; compiled out of
			// release). Defaults to on for Debug and off for RelWithDebInfo - see
			// AE_VALIDATION_DEFAULT_ON in Defines.hpp for why. Either way --validation
			// and --no-validation override it for a single run.
			bool enableValidation = AE_VALIDATION_DEFAULT_ON != 0;

			// Left Windowed unless a host opts in. The editor must never go borderless just
			// because a project asked its game to.
			Window::Mode windowMode = Window::Mode::Windowed;
			// Start unmapped and reveal the window with AetherCore::ShowWindow() when the app
			// is ready to be seen. The editor uses this for the launcher handoff.
			bool startWindowHidden = false;
			// Desktop point to centre the window on before revealing it, so a handoff from
			// another window lands where that window was. INT_MIN means "leave it to the OS".
			int windowCenterX = INT_MIN;
			int windowCenterY = INT_MIN;

			RuntimeProfile profile = RuntimeProfile::Full;
		};

		struct CameraRenderTarget
		{
			uint32_t id = 0;

			[[nodiscard]] bool IsValid() const
			{
				return id != 0;
			}
		};

		explicit AetherCore(const Config& config);
		AetherCore(const Config& config, const EngineSettings& settings);
		~AetherCore() override;

		[[nodiscard]] ServiceContainer& GetServiceContainer()
		{
			return m_services;
		}

		[[nodiscard]] RuntimeProfile GetProfile() const
		{
			return m_profile;
		}

		// Start/stop the dedicated render thread. Start registers the render-thread
		void StartRenderThread();
		void StopRenderThread();

		// Run the producer/game-thread frame loop until the window closes. The
		int RunFrameLoop(EngineClient& client);

		void RunExclusive(QuiesceMode mode, std::function<void()> mutation) override;

		[[nodiscard]] double RealElapsedSeconds() const override
		{
			return m_realElapsedSeconds;
		}

		// The frame loop reads these straight off m_settings, which is a snapshot taken at
		// construction - so without a setter a live change never reaches the loop at all.
		// The producer loop's real period. Distinct from the simulation delta a layer sees,
		// which is snapped to the display cadence and so cannot report a loop running faster
		// than the display - the exact case worth measuring.
		[[nodiscard]] float MedianFrameMs() const;

		// Keeps the editor at its full frame rate for idleAfterSeconds. The engine already
		// counts input as activity; call this for anything else that must keep drawing - play
		// mode, a running animation, a preview that spins - or it will throttle underneath it.
		void RequestActivity() noexcept
		{
			m_lastActivity = std::chrono::steady_clock::now();
		}

		// Input-driven wakefulness, kept separate from RequestActivity on purpose. Input only
		// counts while the window has focus, but WORK has to hold the editor awake whether or
		// not anyone is looking at it: a running game, a publish and a thumbnail bake all keep
		// going after you alt-tab, and throttling them because the window lost focus starves
		// the very thing the user stepped away to wait for.
		void NoteInputActivity() noexcept
		{
			m_lastInputActivity = std::chrono::steady_clock::now();
		}

		// Whether the loop is currently running at the reduced idle rate.
		// Diagnostics for why the editor is or is not idling.
		// Measured input-to-photon latency: from the frame sampling input to that frame
		// being on screen. See PresentTimingTracker.
		[[nodiscard]] float LatchToFlipMs() const;

		// Whether the window had OS focus. Any latency or frame-rate sample is meaningless
		// without it: an unfocused composited window is not throttled by DWM and free-runs to
		// the frame cap, which reads as a large latency win that vanishes the moment anyone
		// actually looks at the window. Two contradictory A/B results came from not recording
		// this.
		[[nodiscard]] bool IsWindowFocused();

		// Monotonic count of focus transitions. A benchmark reads it either side of a
		// measurement window: if it moved, the sample spans two different throttling regimes
		// and has to be thrown away rather than compared.
		[[nodiscard]] std::uint32_t FocusChangeCount() const noexcept
		{
			return m_focusChanges;
		}

		[[nodiscard]] float SecondsSinceActivity() const noexcept
		{
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_lastActivity).count();
		}

		[[nodiscard]] bool IsIdleThrottleAllowed() const noexcept
		{
			return m_idleAllowed;
		}

		[[nodiscard]] bool IsIdleThrottled() const noexcept
		{
			return m_idleThrottled;
		}

		// Opt-in, and off unless a host turns it on. A published game must never throttle
		// itself: it has animation, physics and audio that continue whether or not anyone is
		// touching the keyboard, and "no input" says nothing about whether it should be
		// drawing. Only an editor knows it is safe.
		void SetIdleThrottleAllowed(bool allowed) noexcept
		{
			m_idleAllowed = allowed;
		}

		void SetIdleFps(float fps) noexcept
		{
			m_settings.app.idleFps = fps;
		}

		void SetIdleAfterSeconds(float seconds) noexcept
		{
			m_settings.app.idleAfterSeconds = seconds;
		}

		void SetSyncSlackMs(float slackMs)
		{
			m_settings.graphics.syncSlackMs = slackMs;
		}

		void SetLatencyPacing(bool enabled)
		{
			m_settings.graphics.latencyPacing = enabled;
		}

		void SetTargetFps(float fps)
		{
			m_framePacer.SetTargetFps(fps);
		}

		[[nodiscard]] float GetTargetFps() const
		{
			return m_framePacer.GetTargetFps();
		}

		// recreate (present-mode change) on the next producer-thread poll. No-op
		void SetVsync(bool enabled);
		void SetRenderScale(float scale);

		// Switch presentation live. The window keeps its identity - and so does every Vulkan
		// object tied to it except the swapchain, which the resize path recreates.
		void SetWindowMode(Window::Mode mode);

		// Anisotropic filtering level. Records the request; the sampler is rewritten on the
		// next quiesced frame, because doing it here would call vkDeviceWaitIdle from
		// whichever thread changed the setting while the render thread is still submitting.
		void SetAnisotropy(int anisotropy);

		// Whether the render graph may schedule passes on a dedicated compute queue. Applied
		// on the next quiesced frame, which then rebuilds the graph around the new answer.
		void SetAsyncCompute(bool enabled);

		// MAILBOX instead of FIFO while vsync is on. Same mechanism as SetVsync, because it
		// decides the same thing: the present mode the swapchain is built with. Without
		// this the setting only took hold if you happened to toggle vsync afterwards.
		void SetLowLatencyPresent(bool enabled);

		[[nodiscard]] bool IsVsyncEnabled() const
		{
			return m_settings.graphics.vsync;
		}

		// Enables/disables ImGui multi-viewport at runtime (producer thread); forwards to
		void SetImguiViewportsEnabled(bool enabled);

		// Applies the manual editor UI scale at runtime (producer thread).
		void SetUiScale(float uiScale);

		// GameRuntime never calls it, so the engine never links or initializes
		void SetUiOverlay(std::unique_ptr<IUiOverlay> overlay);

		// pool (render thread, RenderThread::ThreadLoop). No-op when no overlay
		void RecycleUiOverlayFrameData(std::unique_ptr<IUiOverlayFrameData> frame);

		// Frame lifecycle steps (used by the loop and the render thread).
		[[nodiscard]] bool ShouldClose();
		void PumpEvents();
		void Tick(float dt);
		[[nodiscard]] RenderFramePacket PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex);
		void ExecuteRenderFrame(const RenderFramePacket& packet);
		void DiscardPendingFrameQueues(const RenderFramePacket& packet);
		void DiscardAllPendingFrameQueues();
		void WaitIdle();

		// --- Main-thread quiesced swapchain / scene-viewport recreate ----------
		[[nodiscard]] bool NeedsSwapchainOrViewportRecreate();
		void RecreateSwapchainAndResources();

		// Rewrites the bindless sampler when a new anisotropy has been requested. Must only
		// be called with the render thread parked and the GPU idle. Returns whether it ran.
		bool ApplyPendingAnisotropy();

		// Enables or disables async compute when one has been requested. Same preconditions
		// as ApplyPendingAnisotropy. Returns whether the graph now needs rebuilding.
		bool ApplyPendingAsyncCompute();
		void FlushImguiPendingTextureReleases();

		[[nodiscard]] static GpuFormat GetForwardColorFormat();

		[[nodiscard]] std::vector<DebugVertex>& GetPendingDebugVertices()
		{
			return m_pendingDebugVertices;
		}

	private:
		void BeginFrame();
		void EndFrame(const RenderFramePacket& packet);
		void RecreateSwapchain();

		[[nodiscard]] std::vector<std::string> GetRenderPassNames() const;
		[[nodiscard]] std::size_t GetRenderPassCount() const;

		// Requested anisotropy waiting to be applied, or 0 for none. Atomic because the
		// setting can change on any thread while the render loop reads it.
		std::atomic<int> m_pendingAnisotropy{0};
		// Requested async-compute state: +1 on, -1 off, 0 nothing pending. A tri-state
		// rather than a bool pair, because "no request" and "requested off" are different.
		std::atomic<int> m_pendingAsyncCompute{0};

		void BuildShadowsAndRunLighting(const RenderFramePacket& packet, std::uint32_t frameIdx, FrameConstants& fc);
		void PatchShadowIndices(std::uint32_t frameIdx);
		void UploadFrameConstantsAndExecuteRenderGraph(std::uint32_t frameIdx, FrameConstants fc);
		void SubmitAndAdvance(std::uint32_t frameIdx);

		ServiceContainer m_services;
		std::vector<DebugVertex> m_pendingDebugVertices;

		std::unique_ptr<GpuDevice> m_gpu;
		ScreenshotService m_screenshotService;
		CustomPassRegistry m_customPasses; // game-thread project-pass registry + submissions, copied into the packet each frame
		ui::CursorService m_cursor; // engine-drawn mouse pointer; projects configure it via settings
		Light2DSubmissionRegistry m_light2DSubmissions; // script-submitted 2D lights/occluders, drained into the packet each frame
		std::unique_ptr<CameraSubsystem> m_cameras;
		std::unique_ptr<IUiOverlay> m_uiOverlay;
		std::unique_ptr<RenderingSubsystem> m_rendering;
		std::unique_ptr<audio::AudioSubsystem> m_audio;

		RuntimeProfile m_profile = RuntimeProfile::Full;

		// Producer/game-thread frame loop state (distinct from the render-side
		RenderThread m_renderThread;
		FramePacer m_framePacer;
		// Per-frame timings for the Performance panel. Always present, in every build
		// config - the Tracy plots beside it compile out in Release.
		FrameTimeline m_frameTimeline;
		std::chrono::steady_clock::time_point m_lastActivity{std::chrono::steady_clock::now()};
		std::chrono::steady_clock::time_point m_lastInputActivity{std::chrono::steady_clock::now()};
		bool m_idleThrottled = false;
		bool m_idleAllowed = false;
		// Focus transitions since the last frame report. A sample that spans one is comparing
		// two different machines: focused, DWM throttles a composited window to the display
		// rate; unfocused, it free-runs to the frame cap. A latency A/B that changes focus
		// halfway measures the focus change, not the thing under test.
		bool m_lastFocused = false;
		std::uint32_t m_focusChanges = 0;
		std::uint64_t m_producerFrameIndex = 0;
		double m_gameElapsedSeconds = 0.0;
		double m_realElapsedSeconds = 0.0; // wall-clock elapsed, ignores time scale (for pause-menu UI)

		gpu::CommandList m_currentCmdList;
		std::uint64_t m_frameIndex = 0;

		EngineSettings m_settings{};
	};
} // namespace aether
