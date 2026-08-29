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

	class AnimationBlendSystem;

	// The one place settings turn into a present mode, so the three-way choice can never
	// disagree with itself across the call sites that need it.
	[[nodiscard]] constexpr gpu::PresentMode DesiredPresentMode(const EngineSettings& settings) noexcept
	{
		if (!settings.graphics.vsync)
		{
			return gpu::PresentMode::Immediate;
		}
		return settings.graphics.lowLatencyPresent ? gpu::PresentMode::Mailbox : gpu::PresentMode::Fifo;
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

		std::unique_ptr<AnimationBlendSystem> m_animationBlend;

		RuntimeProfile m_profile = RuntimeProfile::Full;

		// Producer/game-thread frame loop state (distinct from the render-side
		RenderThread m_renderThread;
		FramePacer m_framePacer;
		// Per-frame timings for the Performance panel. Always present, in every build
		// config - the Tracy plots beside it compile out in Release.
		FrameTimeline m_frameTimeline;
		std::uint64_t m_producerFrameIndex = 0;
		double m_gameElapsedSeconds = 0.0;
		double m_realElapsedSeconds = 0.0; // wall-clock elapsed, ignores time scale (for pause-menu UI)

		gpu::CommandList m_currentCmdList;
		std::uint64_t m_frameIndex = 0;

		EngineSettings m_settings{};
	};
} // namespace aether
