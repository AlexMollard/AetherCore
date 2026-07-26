#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "EngineClient.hpp"
#include "ui/CursorService.hpp"
#include "IEngineRuntime.hpp"
#include "RuntimeProfile.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/IUiOverlay.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/CustomPassRegistry.hpp"
#include "rendering/Light2DSubmission.hpp"
#include "rendering/RenderThread.hpp"
#include "rendering/ScreenshotService.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/FramePacer.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether
{
	struct FrameConstants;

	class AnimationBlendSystem;
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
			bool enableVsync = true;
			const char* settingsFile = "EngineSettings.toml";
			// GameRuntime must leave this false: Aftermath is a dev tool (dumps
			bool enableGpuDiagnostics = false;
			// Enable the Vulkan validation layer (dev builds only; compiled out of
			// release). Disable it (--no-validation) for a stable high frame rate.
			bool enableValidation = true;

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
		std::uint64_t m_producerFrameIndex = 0;
		double m_gameElapsedSeconds = 0.0;
		double m_realElapsedSeconds = 0.0; // wall-clock elapsed, ignores time scale (for pause-menu UI)

		gpu::CommandList m_currentCmdList;
		std::uint64_t m_frameIndex = 0;

		EngineSettings m_settings{};
	};
} // namespace aether
