#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "EngineClient.hpp"
#include "IEngineRuntime.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderThread.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/FramePacer.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether
{
	struct FrameConstants;

	class AnimationBlendSystem;
	class GpuDevice;
	class CameraSubsystem;
	class ImguiSubsystem;
	class RenderingSubsystem;

	// Owns the whole frame lifecycle: the render thread, the producer/game-thread
	// frame loop, swapchain/viewport recreation, and the exclusive-mutation
	// primitive. The application injects behaviour through EngineClient hooks.
	class AetherCore : public IEngineRuntime
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			int width = 1280;
			int height = 720;
			bool enableVsync = true;
			const char* settingsFile = "engine.toml";
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

		// --- Engine-owned frame lifecycle -------------------------------------
		// Start/stop the dedicated render thread. Start registers the render-thread
		// and IEngineRuntime services; call before layers that consume them attach.
		// Stop joins the thread and waits the GPU idle (call before destroying any
		// GPU-referenced resources, e.g. layer detach).
		void StartRenderThread();
		void StopRenderThread();

		// Run the producer/game-thread frame loop until the window closes. The
		// EngineClient supplies per-frame game logic, UI, and target invalidation.
		int RunFrameLoop(EngineClient& client);

		// IEngineRuntime: quiesce the pipeline, run a mutation exclusively, resume.
		void RunExclusive(QuiesceMode mode, std::function<void()> mutation) override;

		void SetTargetFps(float fps)
		{
			m_framePacer.SetTargetFps(fps);
		}

		[[nodiscard]] float GetTargetFps() const
		{
			return m_framePacer.GetTargetFps();
		}

		// Toggles VSync at runtime: updates the setting and forces a swapchain
		// recreate (present-mode change) on the next producer-thread poll. No-op
		// when unchanged.
		void SetVsync(bool enabled);
		[[nodiscard]] bool IsVsyncEnabled() const
		{
			return m_settings.graphics.vsync;
		}

		// Enables/disables ImGui multi-viewport at runtime (producer thread); forwards to
		// the ImGui subsystem's ViewportsEnable config flag.
		void SetImguiViewportsEnabled(bool enabled);

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
		// The producer (main) thread polls NeedsSwapchainOrViewportRecreate() each
		// iteration; when true it drains + parks the render thread, waits the GPU
		// idle, invalidates retained references, then calls RecreateSwapchainAnd
		// Resources() single-threaded. This replaces the old render-thread-inline
		// recreate that raced ImGui's retained viewport-texture descriptors.
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
		std::unique_ptr<CameraSubsystem> m_cameras;
		std::unique_ptr<ImguiSubsystem> m_imgui;
		std::unique_ptr<RenderingSubsystem> m_rendering;

		std::unique_ptr<AnimationBlendSystem> m_animationBlend;

		// Producer/game-thread frame loop state (distinct from the render-side
		// m_frameIndex below, which is stamped from the packet on the render thread).
		RenderThread m_renderThread;
		FramePacer m_framePacer;
		std::uint64_t m_producerFrameIndex = 0;
		double m_gameElapsedSeconds = 0.0;

		gpu::CommandList m_currentCmdList;
		std::uint64_t m_frameIndex = 0; // render/consumer-side, set from packet.frameIndex

		EngineSettings m_settings{};
	};
} // namespace aether
