#include "AetherCore.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "animation/AnimationBlend.hpp"
#include "animation/AnimationCompiler.hpp"
#include "animation/AnimationDatabase.hpp"

#include "assets/GltfAsset.hpp"
#include "assets/AssetSubsystem.hpp"
#include "camera/CameraSubsystem.hpp"
#include "gpu/AsyncComputeContext.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuProfiler.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/CommandList.hpp"
#include "io/FileSystem.hpp"
#include "material/MaterialBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/EffectParamBuffer.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	AetherCore::AetherCore(const Config& config)
	      : AetherCore(config, EngineSettingsIO::LoadOrCreate(config.settingsFile))
	{
	}

	AetherCore::AetherCore(const Config& config, const EngineSettings& settings)
	{
		io::FileSystem::InitializeDefaultMounts();
		m_profile = config.profile;
		m_settings = settings;
		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		// UiShell (tool front-end, e.g. the project Launcher) skips every scene
		// subsystem: no cameras, lighting, animation, or scene render passes are
		// created and no scene GPU resources are allocated. It brings up only the
		// platform window, the Vulkan device, and the render graph (a single
		// swapchain-clear pass), onto which an editor build installs the ImGui
		// overlay via SetUiOverlay(). See RuntimeProfile.hpp.
		const bool fullRuntime = m_profile == RuntimeProfile::Full;

		// Create all subsystems.
		m_services.Register<AetherCore>(*this);
		m_services.RegisterOwned(std::make_unique<PlatformSubsystem>());
		m_services.RegisterOwned(std::make_unique<SceneSubsystem>());
		m_services.RegisterOwned(std::make_unique<AssetSubsystem>());
		m_services.RegisterOwned(std::make_unique<AsyncComputeContext>());
		m_gpu = std::make_unique<GpuDevice>();
		m_services.Register<GpuDevice>(*m_gpu);
		m_services.Register<ScreenshotService>(m_screenshotService); // Init() below, after the device exists
		if (fullRuntime)
		{
			m_cameras = std::make_unique<CameraSubsystem>();
		}
		m_rendering = std::make_unique<RenderingSubsystem>();

		auto& platform = m_services.Get<PlatformSubsystem>();
		auto& sceneSub = m_services.Get<SceneSubsystem>();
		auto& assetsSub = m_services.Get<AssetSubsystem>();

		// -- 1. Platform -----------------------------------------------------
		platform.Init({.appName = config.appName, .width = config.width, .height = config.height});
		m_services.Register<Window>(platform.GetWindow());
		m_services.Register<Input>(platform.GetInput());

		// -- 2. Graphics device ----------------------------------------------
		AE_EXPECT_OR_THROW_VOID(m_gpu->Init(m_services, {.appName = config.appName, .enableVsync = config.enableVsync, .enableGpuDiagnostics = config.enableGpuDiagnostics}));
		m_screenshotService.Init(m_gpu->GetDevice(), m_gpu->GetGraphicsQueueFamily(), m_gpu->GetGraphicsQueue());
		// Capture whole-frame screenshots from inside the frame command buffer while
		// the image is still owned + in COLOR_ATTACHMENT, not after present.
		m_gpu->GetSwapchain().SetPrePresentCapture([this](void* cmd, void* image, gpu::Extent2D extent) { m_screenshotService.RecordFrameCapture(cmd, image, extent, m_gpu->GetSwapchainColorFormat()); });

		// -- 3. Scene (ECS + legacy) -----------------------------------------
		sceneSub.Init();
		m_services.Register<World>(sceneSub.GetWorld());

		// -- 4. Assets -------------------------------------------------------
		assetsSub.Init(m_services);
		m_services.Register<AssetManager>(assetsSub.GetAssetManager());
		m_services.Register<MeshArena>(assetsSub.GetMeshArena());
		m_services.Register<MeshUploadQueue>(assetsSub.GetMeshUploadQueue());
		m_services.Register<MaterialBuffer>(assetsSub.GetMaterialBuffer());
		m_services.Register<EffectParamBuffer>(assetsSub.GetEffectParamBuffer());
		m_services.Register<MaterialRegistry>(assetsSub.GetMaterialRegistry());
		m_services.Register<AssetSubsystem>(assetsSub);

		// -- 5. Cameras ------------------------------------------------------
		// Scene-only: the CameraManager/LightingManager drive scene rendering and
		// lighting, which UiShell has none of. Skipped there so the render graph
		// never references camera/light state (see RenderingSubsystem::Init).
		if (fullRuntime)
		{
			m_cameras->Init(m_services);
			m_services.Register<CameraManager>(m_cameras->GetCameraManager());
			m_services.Register<LightingManager>(m_cameras->GetLightingManager());
		}

		// -- 6. Rendering ----------------------------------------------------
		m_rendering->Init(m_services, m_profile);
		m_rendering->SetFrameIndexProvider([this]() { return m_frameIndex; });
		m_services.Register<RenderingSubsystem>(*m_rendering);
		m_services.Register<Renderer>(m_rendering->GetRenderer());
		m_services.Register<RenderQueue>(m_rendering->GetRenderQueue());
		m_services.Register<RenderGraph>(m_rendering->GetRenderGraph());
		m_services.Register<ShadowService>(m_rendering->GetShadowService());
		m_services.Register<RenderTargetService>(m_rendering->GetRenderTargetService());

		// -- 7. UI overlay (optional, editor-only) ---------------------------
		// No overlay is created here: the engine core has zero knowledge of any
		// concrete UI toolkit. An editor build (App) constructs one (Dear ImGui's
		// ImguiSubsystem, src/app/imgui/) and installs it via SetUiOverlay()
		// right after this constructor returns - see Application.cpp. The
		// shipped GameRuntime never installs one, so m_uiOverlay stays null and
		// no UI-toolkit code is ever linked or run.

		if (fullRuntime)
		{
			// Link cross-subsystem dependencies.
			m_cameras->GetLightingManager().LinkRenderer(m_rendering->GetRenderer());
			assetsSub.LinkRenderingDeps(m_services);

			// -- 8. Create default main camera -----------------------------------
			auto& cameras = m_services.Get<CameraManager>();
			const CameraHandle mainCam = cameras.Create(CameraDesc{});
			cameras.SetMainCamera(mainCam);

			// -- 9. Async compute (optional) -------------------------------------
			bool enableAsyncCompute = m_settings.graphics.asyncCompute && m_gpu->HasDedicatedComputeQueue();
			if (!m_settings.graphics.asyncCompute)
			{
				AE_INFO(LogCategory::Engine, "Async compute disabled by settings.");
			}
			if (!enableAsyncCompute)
			{
				AE_WARN(LogCategory::Engine, "Async compute disabled: no dedicated compute queue available.");
			}

			if (enableAsyncCompute)
			{
				m_services.Get<AsyncComputeContext>().Init(*m_gpu);
				m_rendering->GetRenderGraph().EnableAsyncCompute(m_gpu->GetComputeQueue(), m_gpu->GetComputeQueueFamily());
			}

			// Register lighting compute passes in the render graph (after async
			// compute enable so they can be scheduled on the async compute queue).
			m_cameras->GetLightingManager().RegisterPasses(m_rendering->GetRenderGraph());
		}

		// Register render-graph passes. In Full this wires the whole scene chain
		// (cull/shadows/GTAO/forward/post-process + overlay); in UiShell it registers
		// only a swapchain-clear pass so the installed ImGui overlay presents over a
		// defined background. Runs in both profiles - RegisterPasses branches on the
		// profile it was Init'd with.
		m_rendering->RegisterPasses(m_services);

		if (fullRuntime)
		{
			// -- 11. Animation systems -------------------------------------------
			m_animationBlend = std::make_unique<AnimationBlendSystem>();

			m_animationBlend->Init(256, 128);

			m_services.Register<AnimationBlendSystem>(*m_animationBlend);

			RenderQueue& rq = m_rendering->GetRenderQueue();
			rq.SetAnimationBlendSystem(m_animationBlend.get());
		}

		// -- 10. Swapchain recreation callback ------------------------------
		m_gpu->SetSwapchainRecreatedCallback([this]() { m_rendering->RecreateSwapchainResources(m_services); });

		AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless sampled-image capacity: {}", m_gpu->GetBindlessManager().GetCapacity());
	}

	AetherCore::~AetherCore()
	{
		// Ensure the render thread is joined before we tear anything down. Stop()
		// is idempotent, so this is safe even if StopRenderThread() already ran;
		// it also guarantees no joinable std::thread at destruction (which would
		// otherwise std::terminate if the loop threw).
		m_renderThread.Stop();

		m_gpu->WaitIdle();
		m_screenshotService.Shutdown();

		m_services.Get<AsyncComputeContext>().Shutdown(*m_gpu);

		// Subsystems free their VMA-backed allocations (VMA still alive).
		m_rendering->Shutdown();
		if (m_uiOverlay)
		{
			m_uiOverlay->Shutdown(m_services);
		}
		if (m_cameras)
		{
			m_cameras->Shutdown();
		}
		m_services.Get<AssetSubsystem>().Shutdown();
		// SceneSubsystem has no shutdown work.

		// Animation systems (reverse of init order). Only constructed in the Full
		// profile (see the fullRuntime guard in Init); the UiShell launcher never
		// creates it, so guard the teardown like the optional subsystems above -
		// otherwise closing the launcher dereferences a null AnimationBlendSystem.
		if (m_animationBlend)
		{
			m_animationBlend->Shutdown();
		}

		// GPU shutdown destroys internal Vulkan resources.
		m_gpu->Shutdown();
		m_services.Get<PlatformSubsystem>().Shutdown();

		m_services.Clear();
		io::FileSystem::Shutdown();
	}

	void AetherCore::WaitIdle()
	{
		m_gpu->WaitIdle();
	}

	void AetherCore::StartRenderThread()
	{
		m_renderThread.Start(*this);
		// Registered before the app attaches layers so consumers (e.g. hot-reload)
		// can resolve them. RenderThread is kept registered for compatibility;
		// IEngineRuntime is the supported surface for app-side exclusive mutations.
		m_services.Register<RenderThread>(m_renderThread);
		m_services.Register<IEngineRuntime>(static_cast<IEngineRuntime&>(*this));
	}

	void AetherCore::StopRenderThread()
	{
		// Join the render thread, THEN drain the GPU. RenderThread::Stop() does not
		// wait the GPU, so callers that destroy GPU-referenced resources afterwards
		// (e.g. layer detach) rely on this WaitIdle.
		m_renderThread.Stop();
		m_gpu->WaitIdle();
	}

	void AetherCore::RunExclusive(QuiesceMode mode, std::function<void()> mutation)
	{
		// Non-reentrant: both callers (recreate, hot-reload) are sequential on the
		// main thread. A mutation that transitively re-enters would prematurely
		// clear the park flag, so fail loudly instead.
		AE_ASSERT_ALWAYS(!m_renderThread.IsReloadInProgress(), "RunExclusive is non-reentrant");

		// (A) DRAIN (Drain mode only, and STRICTLY before parking). With the park
		//     flag not yet set, every submitted frame completes and publishes, so
		//     this cannot hang. Discard mode skips this: the render thread drops
		//     queued frames as it parks (used when the mutation frees resources
		//     those frames reference).
		if (mode == QuiesceMode::Drain && m_producerFrameIndex > 0)
		{
			m_renderThread.WaitUntilFrameCompleted(m_producerFrameIndex - 1);
		}

		// (B) PARK the render thread outside ExecuteRenderFrame.
		m_renderThread.SetReloadInProgress(true);
		m_renderThread.WaitPaused();

		// (C) GPU idle. AetherCore::WaitIdle (m_gpu->WaitIdle), NOT the render
		//     thread's identically-named join method.
		WaitIdle();

		// (D) Mutation runs single-threaded, GPU idle, render thread parked. Any
		//     queue clearing / entity destruction is the mutation's responsibility.
		if (mutation)
		{
			mutation();
		}

		// (F) Resume.
		m_renderThread.SetReloadInProgress(false);
	}

	int AetherCore::RunFrameLoop(EngineClient& client)
	{
		AE_PROFILE_ZONE();
		auto previousFrameTime = std::chrono::steady_clock::now();

		// Dev-only one-shot self-screenshot, independent of any UI toolkit or control
		// endpoint and of screen-lock state (the ScreenshotService captures the
		// swapchain GPU-side, not the desktop). AETHER_SCREENSHOT=<path> captures on
		// frame AETHER_SCREENSHOT_FRAME (default 120, giving fonts/first frames time to
		// settle), then the app keeps running. Works in every RuntimeProfile - this is
		// how the Launcher (which has no control server) can be captured like the
		// editor's viewport.screenshot.
		std::string screenshotPath;
		std::uint64_t screenshotFrame = 120;
		bool screenshotRequested = false;
		const auto readEnvironmentVariable = [](const char* name) -> std::string
		{
#ifdef _MSC_VER
			char* value = nullptr;
			std::size_t size = 0;
			if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
			{
				return {};
			}

			std::string result(value);
			std::free(value);
			return result;
#else
			const char* value = std::getenv(name);
			return value != nullptr ? value : "";
#endif
		};

		const std::string envPath = readEnvironmentVariable("AETHER_SCREENSHOT");
		if (!envPath.empty())
		{
			screenshotPath = envPath;
			const std::string envFrame = readEnvironmentVariable("AETHER_SCREENSHOT_FRAME");
			if (!envFrame.empty())
			{
				try
				{
					screenshotFrame = static_cast<std::uint64_t>(std::stoull(envFrame));
				}
				catch (const std::exception&)
				{
				}
			}
		}

		while (!ShouldClose())
		{
			AE_PROFILE_ZONE_N("Frame");

			m_framePacer.Wait();
			client.OnFrameBegin(); // app drains its coroutine executor here
			Logger::SetFrameNumber(m_producerFrameIndex);

			// Measure dt before PumpEvents so window-event stalls don't inflate it.
			constexpr double kMaxDeltaTime = 1.0 / 30.0;
			const auto now = std::chrono::steady_clock::now();
			const double rawDt = std::min(std::chrono::duration<double>(now - previousFrameTime).count(), kMaxDeltaTime);
			previousFrameTime = now;

			PumpEvents();

			// Handle window resize / scene-viewport rebuild before building a frame
			// so the channel is provably empty when the render thread is parked.
			if (NeedsSwapchainOrViewportRecreate())
			{
				RunExclusive(QuiesceMode::Drain,
				        [this, &client]()
				        {
					        client.OnRenderTargetsInvalidated();
					        FlushImguiPendingTextureReleases();
					        RecreateSwapchainAndResources();
				        });
			}

			// Producer backpressure: stay at most kMaxFramesInFlight ahead.
			if (m_producerFrameIndex >= Swapchain::kMaxFramesInFlight)
			{
				m_renderThread.WaitUntilFrameCompleted(m_producerFrameIndex - Swapchain::kMaxFramesInFlight);
			}

			// Camera/input use UNSCALED dt so they stay controllable during
			// fast-forward; the game clock uses the scaled dt.
			Tick(static_cast<float>(rawDt));
			const double gameDt = rawDt * client.GetTimeScale();
			m_gameElapsedSeconds += gameDt;

			const auto drawSlot = static_cast<std::uint32_t>(m_producerFrameIndex % Swapchain::kMaxFramesInFlight);
			if (m_profile == RuntimeProfile::Full)
			{
				// Reset this frame's double-buffer write slot BEFORE the update so
				// game logic writes its draws into a cleared slot. UiShell submits no
				// scene draws or shadows, so there is nothing to reset.
				auto& renderQueue = m_services.Get<RenderQueue>();
				renderQueue.SetWriteSlot(drawSlot);
				renderQueue.Clear(drawSlot);
				m_services.Get<ShadowService>().PrepareWriteSlot(drawSlot);
			}

			client.OnUpdate(gameDt, m_producerFrameIndex);

			if (m_uiOverlay)
			{
				m_uiOverlay->BeginFrame(m_services, static_cast<float>(rawDt));
			}
			client.OnBuildUI(gameDt, m_producerFrameIndex);

			// Acquire a warm overlay frame-data shell (pooled ImDrawLists under the
			// hood) so capture stays near zero-allocation after the first few
			// growth frames. Stays null when no overlay is installed (GameRuntime).
			std::unique_ptr<IUiOverlayFrameData> overlayFrame;
			if (m_uiOverlay)
			{
				overlayFrame = m_uiOverlay->AcquireFrameData();
				m_uiOverlay->Render();

				// Structural viewport DESTROY (re-dock / close / toggle-off) must retire the
				// render-thread swapchains before UpdatePlatformWindows destroys the GLFW
				// window: park the render thread + idle the GPU via RunExclusive. Create and
				// resize need no quiesce (render-thread-local).
				//
				// Pending backend TEXTURE updates force the same quiesce: SnapshotFrame
				// uploads them via a producer-thread vkQueueSubmit, and vkQueueSubmit is
				// externally synchronized -- submitting while the render thread is also
				// submitting races the queue (spec-level UB; corrupts the validation
				// layer's tracking). Rare (font atlas on the first frames, new textures).
				const std::vector<std::uint32_t> departedViewports = m_uiOverlay->SecondaryViewportIdsWithPendingDestroy();
				if (!departedViewports.empty() || m_uiOverlay->HasPendingTextureUpdates())
				{
					// Release the ImGui frame lock BEFORE quiescing: RunExclusive drains and
					// parks the render thread, which would otherwise deadlock waiting on the
					// mutex this producer thread still holds via the frame lock.
					m_uiOverlay->EndFrameLock();
					RunExclusive(QuiesceMode::Drain,
					        [this, &departedViewports]()
					        {
						        m_uiOverlay->RetireViewports(departedViewports);
						        m_uiOverlay->UpdatePlatformWindows();
					        });
					m_uiOverlay->SnapshotFrame(*overlayFrame); // render thread idle post-quiesce
				}
				else
				{
					m_uiOverlay->UpdatePlatformWindows();
					m_uiOverlay->SnapshotFrame(*overlayFrame);
					m_uiOverlay->EndFrameLock();
				}
			}

			RenderFramePacket packet = PrepareFrame(drawSlot, m_producerFrameIndex);
			packet.elapsedTime = static_cast<float>(m_gameElapsedSeconds);
			packet.uiOverlay = std::move(overlayFrame);

			// Resolve the scene UI (canvases -> draw commands) HERE, on the producer,
			// which owns the ECS - never on the render thread, which runs concurrently
			// and would race a structural registry change (script attach on Play). The
			// draw commands land in this slot's buffer; the render graph's $UiOverlay
			// pass reads that same slot when it executes the submitted packet.
			// Scene game-UI (ECS canvases -> $UiOverlay draw commands) is Full-runtime
			// only. UiShell registers no $UiOverlay pass and has no ECS world; its
			// front-end UI is drawn entirely by the installed Dear ImGui overlay.
			if (m_rendering && m_profile == RuntimeProfile::Full)
			{
				ui::UiRenderer& uiRenderer = m_rendering->GetUiRenderer();
				uiRenderer.SetWorld(&m_services.Get<SceneSubsystem>().GetWorld());
				uiRenderer.BuildFrame({static_cast<float>(packet.renderExtent.width), static_cast<float>(packet.renderExtent.height)}, packet.drawSlot);
			}

			m_renderThread.SubmitFrame(std::move(packet));

			if (!screenshotPath.empty() && !screenshotRequested && m_producerFrameIndex >= screenshotFrame && m_screenshotService.IsInitialized())
			{
				(void) m_screenshotService.Request(screenshotPath);
				screenshotRequested = true;
				AE_INFO(LogCategory::Engine, "Self-screenshot requested (frame {}) -> {}", m_producerFrameIndex, screenshotPath);
			}

			++m_producerFrameIndex;
		}

		Logger::ClearFrameNumber();
		return 0;
	}

	bool AetherCore::ShouldClose()
	{
		return m_services.Get<PlatformSubsystem>().GetWindow().ShouldClose();
	}

	void AetherCore::PumpEvents()
	{
		m_services.Get<PlatformSubsystem>().GetWindow().PollEvents();
	}

	void AetherCore::Tick(const float dt)
	{
		AE_PROFILE_ZONE();
		auto& platform = m_services.Get<PlatformSubsystem>();
		auto& input = platform.GetInput();
		input.Update();
		if (m_uiOverlay)
		{
			input.SetMouseCaptured(m_uiOverlay->WantsInputCapture() && !input.IsMouseViewportInputActive());
		}
		// Scene camera navigation is Full-runtime only; UiShell has no cameras.
		if (m_cameras)
		{
			m_cameras->GetCameraManager().Update(input, dt);
		}
	}

	void AetherCore::BeginFrame()
	{
		AE_PROFILE_ZONE();
		// Swapchain / scene-viewport recreation is NOT done here. It is driven from
		// the producer (main) thread via NeedsSwapchainOrViewportRecreate() +
		// RecreateSwapchainAndResources() while the render thread is parked, so no
		// in-flight packet's retained ImGui descriptor can reference a resource
		// this thread just destroyed. If the swapchain is out of date, acquire
		// below marks it and BeginSwapchainFrame produces an invalid frame that
		// EndFrame cleanly discards + advances; the main thread recreates next
		// iteration.
		m_gpu->BeginSwapchainFrame();
		m_currentCmdList = gpu::CommandList(m_gpu->GetSwapchain().GetCurrentCommandBuffer());
	}

	bool AetherCore::NeedsSwapchainOrViewportRecreate()
	{
		const bool framebufferResized = m_services.Get<PlatformSubsystem>().GetWindow().PeekFramebufferResized();
		const bool swapchainOutOfDate = m_gpu->SwapchainNeedsRecreation();
		const bool viewportPending = m_rendering != nullptr && m_rendering->IsSceneViewportRebuildPending();
		return framebufferResized || swapchainOutOfDate || viewportPending;
	}

	void AetherCore::RecreateSwapchainAndResources()
	{
		// Consume the resize triggers. The swapchain flag is cleared inside
		// RecreateSwapchain()/GpuDevice::RecreateSwapchain (ClearRecreationFlag);
		// the framebuffer-resized flag is exchanged here.
		const bool framebufferResized = m_services.Get<PlatformSubsystem>().GetWindow().ConsumeFramebufferResized();
		const bool swapchainDirty = framebufferResized || m_gpu->SwapchainNeedsRecreation();

		// Commit any pending scene-viewport settings first so the resource rebuild
		// uses the new extent. Sole consumer of the pending flag.
		const bool viewportCommitted = m_rendering != nullptr && m_rendering->CommitPendingSceneViewportSettings();

		if (swapchainDirty)
		{
			// Recreates the VkSwapchain (new extent) and fires the recreated
			// callback -> RenderingSubsystem::RecreateSwapchainResources.
			RecreateSwapchain();
		}
		else if (viewportCommitted && m_rendering != nullptr)
		{
			// Viewport-only change (toggle / resolution): rebuild extent-dependent
			// resources without touching the swapchain.
			m_rendering->RecreateSwapchainResources(m_services);
		}
	}

	void AetherCore::FlushImguiPendingTextureReleases()
	{
		if (m_uiOverlay)
		{
			m_uiOverlay->FlushPendingTextureReleasesImmediate();
		}
	}

	std::vector<std::string> AetherCore::GetRenderPassNames() const
	{
		if (!m_rendering)
		{
			return {};
		}
		const auto passes = m_rendering->GetRenderGraph().GetPasses();
		std::vector<std::string> names;
		names.reserve(passes.size());
		for (const auto& pass: passes)
		{
			names.push_back(pass.name);
		}
		return names;
	}

	std::size_t AetherCore::GetRenderPassCount() const
	{
		if (!m_rendering)
		{
			return 0;
		}
		return m_rendering->GetRenderGraph().GetPasses().size();
	}

	void AetherCore::SetVsync(bool enabled)
	{
		if (m_settings.graphics.vsync == enabled)
		{
			return;
		}
		m_settings.graphics.vsync = enabled;
		if (m_gpu)
		{
			// RecreateSwapchain() reads m_settings.graphics.vsync; requesting a
			// recreate makes the producer thread pick up the new present mode.
			m_gpu->RequestSwapchainRecreation();
		}
		AE_INFO(LogCategory::Engine, "VSync {}.", enabled ? "enabled" : "disabled");
	}

	void AetherCore::SetImguiViewportsEnabled(bool enabled)
	{
		// A UiShell app is a single-window tool front end (e.g. the Launcher): ImGui
		// multi-viewport (tear-out OS windows) makes no sense there, and would float
		// the overlay's windows out of the main swapchain - leaving it empty for the
		// ScreenshotService and anything else that reads it. Force it off regardless of
		// the requested value / the graphics.imguiViewports setting.
		if (m_profile != RuntimeProfile::Full)
		{
			enabled = false;
		}
		m_settings.graphics.imguiViewports = enabled;
		if (m_uiOverlay)
		{
			m_uiOverlay->SetViewportsEnabled(enabled);
		}
	}

	void AetherCore::SetUiScale(float uiScale)
	{
		m_settings.graphics.uiScale = uiScale;
		if (m_uiOverlay)
		{
			m_uiOverlay->SetUiScale(uiScale);
		}
	}

	void AetherCore::SetUiOverlay(std::unique_ptr<IUiOverlay> overlay)
	{
		m_uiOverlay = std::move(overlay);
		if (m_uiOverlay)
		{
			m_uiOverlay->Init(m_services);
		}
	}

	void AetherCore::RecycleUiOverlayFrameData(std::unique_ptr<IUiOverlayFrameData> frame)
	{
		if (m_uiOverlay && frame)
		{
			m_uiOverlay->RecycleFrameData(std::move(frame));
		}
	}

	void AetherCore::RecreateSwapchain()
	{
		auto& platform = m_services.Get<PlatformSubsystem>();
		auto size = platform.GetWindow().WaitForValidFramebufferSize();
		m_gpu->RecreateSwapchain(platform.GetWindow(), m_settings.graphics.vsync);

		AE_INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", size.width, size.height);
	}

	RenderFramePacket AetherCore::PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();

		// UiShell has no scene to extract: no world flush, cameras, lights, shadows,
		// or physics-debug shapes. The render graph runs a single swapchain-clear
		// pass and the installed ImGui overlay draws on top (see EndFrame). Return a
		// minimal packet carrying only the frame identity and the swapchain extent.
		if (m_profile != RuntimeProfile::Full)
		{
			RenderFramePacket packet;
			packet.frameIndex = frameIndex;
			packet.drawSlot = drawSlot;
			packet.renderExtent = m_gpu->GetSwapchainExtent();
			return packet;
		}

		auto& assetsSub = m_services.Get<AssetSubsystem>();

		gpu::Extent2D extent = m_rendering->ResolveRequestedSceneViewportExtent(m_gpu->GetSwapchainExtent());
		if (extent.width == 0 || extent.height == 0)
		{
			extent = m_gpu->GetSwapchainExtent();
		}
		RenderQueue& renderQueue = m_rendering->GetRenderQueue();
		World& world = m_services.Get<SceneSubsystem>().GetWorld();
		RenderTargetService& rttService = m_rendering->GetRenderTargetService();
		ShadowService& shadowService = m_rendering->GetShadowService();
		LocalShadowService& localShadowService = m_rendering->GetLocalShadowService();
		MaterialBuffer& materialBuffer = assetsSub.GetMaterialBuffer();
		EffectParamBuffer& effectParamBuffer = assetsSub.GetEffectParamBuffer();
		CameraManager& cameras = m_cameras->GetCameraManager();
		Renderer& renderer = m_rendering->GetRenderer();

		renderQueue.SetWriteSlot(drawSlot);
		WorldRenderer::Flush(world, renderQueue);

		rttService.PrepareQueues(drawSlot, world);

		const glm::vec4 sunDirIntensity = renderer.GetDirectionalLightVector();
		const bool directionalShadowEnabled = sunDirIntensity.w > 0.001f;
		shadowService.SetDirectionalShadowEnabled(directionalShadowEnabled);
		shadowService.PrepareQueues(drawSlot, world);
		localShadowService.PrepareQueues(drawSlot, world);
		m_rendering->GetCameraPreview().PrepareQueue(drawSlot, world);
		m_rendering->GetModelPreview().PrepareQueue(drawSlot);

		RenderFramePacket packet;
		packet.frameIndex = frameIndex;
		packet.drawSlot = drawSlot;
		packet.renderExtent = extent;
		packet.materialBufferAddr = materialBuffer.GetDeviceAddressU64();
		packet.effectParamBufferAddr = effectParamBuffer.GetDeviceAddressU64();

		if (const Camera* cam = cameras.TryGetMainCamera())
		{
			packet.hasCameraData = true;
			packet.view = cam->GetViewMatrix();
			const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
			packet.proj = cam->GetProjectionMatrix(aspect);
			packet.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
			packet.cameraNearPlane = cam->GetNearPlane();
		}

		packet.sunDirectionIntensity = sunDirIntensity;
		packet.directionalShadowEnabled = directionalShadowEnabled;
		packet.ambientColor = renderer.GetAmbientLightVector();
		packet.sunColor = renderer.GetSunColorVector();
		packet.skyHorizonColor = renderer.GetSkyHorizonColorVector();
		packet.skyZenithColor = renderer.GetSkyZenithColorVector();
		packet.skyVoidColor = renderer.GetSkyVoidColorVector();

		packet.pointLights.assign(renderer.GetPointLights().begin(), renderer.GetPointLights().end());

		// Hand the game-thread debug vertex buffer to the packet. This is the
		// synchronization point with the render thread: the channel transfer
		// of the packet (in Application::SubmitFrame) takes ownership of the
		// moved vector, so no locks are required.
		packet.debugVertices = std::move(m_pendingDebugVertices);
		m_pendingDebugVertices.clear();
		packet.spotLights.assign(renderer.GetSpotLights().begin(), renderer.GetSpotLights().end());

		// Debug-viz toggles are read HERE (producer) and expressed as packet data:
		// the master switch as a bool the pass branches on, the collider shapes as
		// presence-or-absence. The render thread reads neither global.
		packet.debugRenderingEnabled = IsDebugRenderingEnabled();
		m_rendering->GetPhysicsDebugRenderer().ExtractShapes(world, packet.physicsDebugShapes);

		return packet;
	}

	void AetherCore::ExecuteRenderFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		const auto execStart = std::chrono::steady_clock::now();
		m_frameIndex = packet.frameIndex;
		BeginFrame();

		// INVARIANT: the render thread reads ONLY `packet`, never the live ECS. All
		// world-derived data (draw lists, lights, shadows, UI, physics-debug shapes)
		// is extracted on the producer in PrepareFrame. Do NOT add a
		// SceneSubsystem::GetWorld() here - the producer runs up to
		// kMaxFramesInFlight ahead and can structurally mutate the registry (e.g.
		// script attach on Play), which would race an entt read on this thread.
		// See docs/architecture/render-frame-extraction.md.
		if (m_rendering)
		{
			PhysicsDebugRenderer& debugRenderer = m_rendering->GetPhysicsDebugRenderer();
			debugRenderer.SetFrameDebugVertices(&packet.debugVertices);
			debugRenderer.SetFramePhysicsShapes(&packet.physicsDebugShapes);
			debugRenderer.SetFrameDebugEnabled(packet.debugRenderingEnabled);
		}

		EndFrame(packet);

		// After the frame is presented, service any pending screenshot request
		// (render thread owns the queue; the readback is self-contained).
		if (m_screenshotService.IsInitialized())
		{
			m_screenshotService.ProcessPending(static_cast<void*>(m_gpu->GetSwapchain().GetCurrentImage()), m_gpu->GetSwapchainExtent(), m_gpu->GetSwapchainColorFormat());
		}

		AE_PROFILE_PLOT("Frame/RenderThreadExecNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - execStart).count()));
	}

	void AetherCore::DiscardPendingFrameQueues(const RenderFramePacket& packet)
	{
		if (m_rendering)
		{
			m_rendering->DiscardPendingFrameQueues(packet.drawSlot);
		}
	}

	void AetherCore::DiscardAllPendingFrameQueues()
	{
		if (!m_rendering)
		{
			return;
		}

		for (std::uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot)
		{
			m_rendering->DiscardPendingFrameQueues(slot);
		}
	}

	void AetherCore::EndFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		const auto frameIdx = static_cast<std::uint32_t>(packet.drawSlot % kMaxFramesInFlight);
		AE_ASSERT_ALWAYS(frameIdx == static_cast<std::uint32_t>(packet.frameIndex % kMaxFramesInFlight), "Render frame packet slot/index mismatch.");

		if (!m_gpu->IsSwapchainFrameValid())
		{
			DiscardPendingFrameQueues(packet);
			m_gpu->SubmitAndPresent();
			AE_PROFILE_FRAME;
			++m_frameIndex;
			m_gpu->GetBindlessManager().AdvanceFrame(m_frameIndex);
			m_services.Get<AssetSubsystem>().AdvanceFrame(m_frameIndex);
			m_gpu->AdvanceResourceRegistryFrame();
			return;
		}

		FrameConstants fc = m_gpu->ComposeBaseFrameConstants(packet, glm::mat4(1.0f));

		// Shadows/lighting/shadow-index patching are scene work (they read cameras,
		// lights, and the shadow services). UiShell has none of these; it applies the
		// no-camera lighting fallback so the frame constants are well-formed for the
		// clear pass, then runs the (UI-only) render graph.
		if (m_profile == RuntimeProfile::Full)
		{
			BuildShadowsAndRunLighting(packet, frameIdx, fc);

			if (packet.hasCameraData)
			{
				PatchShadowIndices(frameIdx);
			}
		}
		else
		{
			m_gpu->ApplyNoCameraLightingFallback(fc);
		}

		UploadFrameConstantsAndExecuteRenderGraph(frameIdx, fc);
		if (m_uiOverlay && packet.uiOverlay)
		{
			m_uiOverlay->RenderFrame(*packet.uiOverlay, m_currentCmdList, m_gpu->BuildFrameTarget());
		}

		SubmitAndAdvance(frameIdx);

		// Secondary (torn-out) viewports render + present on the render thread, after the
		// main frame's render graph produced any images they sample (e.g. the Viewport
		// panel's final-color image) — correct GPU synchronization, unlike a producer-side
		// present.
		if (m_uiOverlay && packet.uiOverlay)
		{
			m_uiOverlay->RenderViewports(*packet.uiOverlay);
		}
	}

	void AetherCore::BuildShadowsAndRunLighting(const RenderFramePacket& packet, std::uint32_t frameIdx, FrameConstants& fc)
	{
		m_rendering->GetShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras->GetCameraManager(), fc);
		m_rendering->GetLocalShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras->GetCameraManager(), fc);

		if (packet.hasCameraData)
		{
			auto& lightingMgr = m_cameras->GetLightingManager();
			const gpu::Extent2D lightingExtent = packet.renderExtent.width != 0 && packet.renderExtent.height != 0 ? packet.renderExtent : m_gpu->GetSwapchainExtent();
			const bool lightDataReady = lightingMgr.PrepareForRenderGraph(frameIdx, packet.view, packet.proj, packet.cameraNearPlane, lightingExtent, fc, packet.pointLights, packet.spotLights);

			if (!lightDataReady)
			{
				m_gpu->ApplyNoCameraLightingFallback(fc);
			}

			// Update render graph buffer handles for the current frame's lighting buffers.
			lightingMgr.UpdateBufferHandles(m_rendering->GetRenderGraph(), frameIdx);
		}
		else
		{
			m_gpu->ApplyNoCameraLightingFallback(fc);
		}
	}

	void AetherCore::PatchShadowIndices(const std::uint32_t frameIdx)
	{
		m_cameras->GetLightingManager().ApplyShadowIndices(frameIdx, m_rendering->GetLocalShadowService().GetLightShadowIndices());
	}

	void AetherCore::UploadFrameConstantsAndExecuteRenderGraph(std::uint32_t frameIdx, FrameConstants fc)
	{
		const bool fullRuntime = m_profile == RuntimeProfile::Full;

		// Update render graph buffer handles for the current frame's histogram
		// readback buffer and collect completed histogram data. Post-process is a
		// scene resource (not created in UiShell).
		if (fullRuntime)
		{
			m_rendering->GetPostProcessStack().UpdateBufferHandles(m_rendering->GetRenderGraph(), frameIdx);
		}

		const FrameTarget frameTarget = m_gpu->BuildFrameTarget();
		const std::uint64_t frameAddr = m_rendering->GetFrameConstantsBuffer().GetDeviceAddressU64(frameIdx);
		const FrameResourceContext frameContext{
		        .target = frameTarget,
		        .extent = frameTarget.extent,
		        .frameIndex = m_frameIndex,
		        .frameSlot = frameIdx,
		        .swapchainImageIndex = m_gpu->GetCurrentSwapchainImageIndex(),
		        .frameConstantsAddr = frameAddr,
		};
		auto& blackboard = m_rendering->GetRenderGraph().GetBlackboard();
		(void) blackboard.SetFrameProduct<MainViewProduct>(std::string{kFrameProductMainView},
		        MainViewProduct{
		                .extent = frameTarget.extent,
		                .frameIndex = m_frameIndex,
		                .frameSlot = frameIdx,
		                .frameConstantsAddr = frameAddr,
		        },
		        FrameBlackboard::ProductMetadata{
		                .frameSlot = frameIdx,
		                .extent = frameTarget.extent,
		        });

		m_currentCmdList.BeginDebugLabel("Frame.RenderGraph", 0.35f, 0.55f, 0.95f, 1.0f);
		m_rendering->GetRenderGraph().BeginFrame(frameIdx);
		fc.resourceTableAddr = static_cast<std::uint64_t>(m_rendering->PublishFrameResourceTable(frameIdx));
		m_rendering->GetFrameConstantsBuffer().Write(frameIdx, fc);
		// Build the camera-preview frame constants from the same fc (keeps shadow /
		// resource-table addresses), overriding the camera to the preview POV and
		// staging the preview's own light binning. Editor previews are scene
		// features (they read the lighting manager); UiShell has neither.
		if (fullRuntime)
		{
			m_rendering->GetCameraPreview().BuildFrameConstants(fc, frameIdx, &m_cameras->GetLightingManager());
			m_rendering->GetModelPreview().BuildFrameConstants(fc, frameIdx);
		}
		m_currentCmdList.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::AllCommands, gpu::AccessFlags::ShaderRead | gpu::AccessFlags::ShaderWrite);
		m_rendering->GetRenderGraph().Execute(m_currentCmdList, frameContext);
		m_currentCmdList.EndDebugLabel();
	}

	void AetherCore::SubmitAndAdvance(const std::uint32_t frameIdx)
	{
		auto& renderGraph = m_rendering->GetRenderGraph();

		// Submit the async compute command buffer now, right before the graphics
		// submission, so both queues are dispatched to the GPU simultaneously.
		// The graphics submission waits on the compute timeline semaphore,
		// ensuring the GPU sees compute results before draw-indirect.
		renderGraph.SubmitComputeWork(frameIdx);

		const gpu::TimelineSemaphoreHandle graphAsyncSem = renderGraph.HasAsyncComputeWork() ? renderGraph.GetComputeTimelineSemaphore() : nullptr;
		const std::uint64_t graphAsyncVal = renderGraph.HasAsyncComputeWork() ? renderGraph.GetComputeTimelineValue() : 0;

		m_gpu->SubmitAndPresent(graphAsyncSem, graphAsyncVal);

		// Collect Tracy GPU timestamps AFTER submission so the query pool
		// contains valid GPU data. Collecting before submission reads stale
		// results and wraps the pool before the GPU has written, triggering
		// "query not reset" validation errors.
		// Pass nullptr for host-side query pool reset (TracyVkContextHostCalibrated
		// was used at init). Passing the submitted command buffer would issue
		// vkCmdResetQueryPool on a PENDING buffer, which is invalid.
		gpu::GpuProfiler::Get().Collect(nullptr);

		AE_PROFILE_FRAME;
		++m_frameIndex;
		m_gpu->GetBindlessManager().AdvanceFrame(m_frameIndex);
		m_services.Get<AssetSubsystem>().AdvanceFrame(m_frameIndex);
		m_gpu->AdvanceResourceRegistryFrame();
	}

	GpuFormat AetherCore::GetForwardColorFormat()
	{
		return GpuDevice::GetForwardColorFormat();
	}

} // namespace aether
