#include "AetherCore.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <span>
#include <vector>
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
#include "particles/ParticleSystem.hpp"
#include "physics2d/Physics2DDebugDraw.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/LightComponents.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/FrameStats.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	AetherCore::AetherCore(const Config& config)
	      : AetherCore(config, EngineSettingsIO::LoadOrCreate(config.settingsFile))
	{
	}

	AetherCore::AetherCore(const Config& config, const EngineSettings& settings)
	      : m_profile(config.profile), m_settings(settings)
	{
		io::FileSystem::InitializeDefaultMounts();

		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		const bool fullRuntime = m_profile == RuntimeProfile::Full;

		m_services.Register<AetherCore>(*this);
		m_services.Register<FrameTimeline>(m_frameTimeline);
		m_services.RegisterOwned(std::make_unique<PlatformSubsystem>());
		m_services.RegisterOwned(std::make_unique<SceneSubsystem>());
		m_services.RegisterOwned(std::make_unique<AssetSubsystem>());
		m_services.RegisterOwned(std::make_unique<AsyncComputeContext>());
		m_gpu = std::make_unique<GpuDevice>();
		m_services.Register<GpuDevice>(*m_gpu);
		m_services.Register<ScreenshotService>(m_screenshotService);
		m_services.Register<CustomPassRegistry>(m_customPasses);
		m_services.Register<Light2DSubmissionRegistry>(m_light2DSubmissions);
		m_services.Register<ui::CursorService>(m_cursor);

		// A project turns its own pointer on and names its art in ProjectSettings.toml - no code.
		m_cursor.Configure(m_settings.cursor.custom,
		        {
		                .texture = m_settings.cursor.texture,
		                .hotspot = {m_settings.cursor.hotspotX, m_settings.cursor.hotspotY},
		                .size = m_settings.cursor.size,
		                .pixelArt = m_settings.cursor.pixelArt,
		        });
		if (fullRuntime)
		{
			m_cameras = std::make_unique<CameraSubsystem>();
		}
		m_rendering = std::make_unique<RenderingSubsystem>();

		auto& platform = m_services.Get<PlatformSubsystem>();
		auto& sceneSub = m_services.Get<SceneSubsystem>();
		auto& assetsSub = m_services.Get<AssetSubsystem>();

		platform.Init({.appName = config.appName, .width = config.width, .height = config.height});
		m_services.Register<Window>(platform.GetWindow());
		m_services.Register<Input>(platform.GetInput());

		AE_EXPECT_OR_THROW_VOID(m_gpu->Init(m_services, {.appName = config.appName, .enableVsync = config.enableVsync, .enableGpuDiagnostics = config.enableGpuDiagnostics, .enableValidation = config.enableValidation}));
		m_screenshotService.Init(m_gpu->GetDevice(), m_gpu->GetGraphicsQueueFamily(), m_gpu->GetGraphicsQueue());
		m_gpu->GetSwapchain().SetPrePresentCapture([this](void* cmd, void* image, gpu::Extent2D extent) { m_screenshotService.RecordFrameCapture(cmd, image, extent, m_gpu->GetSwapchainColorFormat()); });

		aether::SceneSubsystem::Init();
		m_services.Register<World>(sceneSub.GetWorld());

		assetsSub.Init(m_services);
		m_services.Register<AssetManager>(assetsSub.GetAssetManager());
		m_services.Register<MaterialBuffer>(assetsSub.GetMaterialBuffer());
		m_services.Register<EffectParamBuffer>(assetsSub.GetEffectParamBuffer());
		m_services.Register<MaterialRegistry>(assetsSub.GetMaterialRegistry());
		m_services.Register<AssetSubsystem>(assetsSub);

		// never references camera/light state (see RenderingSubsystem::Init).
		if (fullRuntime)
		{
			m_cameras->Init(m_services);
			m_services.Register<CameraManager>(m_cameras->GetCameraManager());
			m_services.Register<LightingManager>(m_cameras->GetLightingManager());
		}

		m_rendering->Init(m_services, m_profile);
		m_rendering->SetFrameIndexProvider([this]() { return m_frameIndex; });
		m_services.Register<RenderingSubsystem>(*m_rendering);
		m_services.Register<Renderer>(m_rendering->GetRenderer());
		m_services.Register<RenderQueue>(m_rendering->GetRenderQueue());
		m_services.Register<RenderGraph>(m_rendering->GetRenderGraph());
		m_services.Register<ShadowService>(m_rendering->GetShadowService());
		m_services.Register<RenderTargetService>(m_rendering->GetRenderTargetService());

		// shipped GameRuntime never installs one, so m_uiOverlay stays null and

		if (fullRuntime)
		{
			m_cameras->GetLightingManager().LinkRenderer(m_rendering->GetRenderer());
			assetsSub.LinkRenderingDeps(m_services);

			auto& cameras = m_services.Get<CameraManager>();
			const CameraHandle mainCam = cameras.Create(CameraDesc{});
			cameras.SetMainCamera(mainCam);

			const bool enableAsyncCompute = m_settings.graphics.asyncCompute && m_gpu->HasDedicatedComputeQueue();
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

			m_cameras->GetLightingManager().RegisterPasses(m_rendering->GetRenderGraph());
		}

		m_rendering->RegisterPasses(m_services);

		if (fullRuntime)
		{
			m_animationBlend = std::make_unique<AnimationBlendSystem>();

			m_animationBlend->Init(256, 128);

			m_services.Register<AnimationBlendSystem>(*m_animationBlend);

			RenderQueue& rq = m_rendering->GetRenderQueue();
			rq.SetAnimationBlendSystem(m_animationBlend.get());
		}

		m_gpu->SetSwapchainRecreatedCallback([this]() { m_rendering->RecreateSwapchainResources(m_services); });

		AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless sampled-image capacity: {}", m_gpu->GetBindlessManager().GetCapacity());
	}

	AetherCore::~AetherCore()
	{
		// Ensure the render thread is joined before we tear anything down. Stop()
		m_renderThread.Stop();

		m_gpu->WaitIdle();
		m_screenshotService.Shutdown();

		m_services.Get<AsyncComputeContext>().Shutdown(*m_gpu);

		m_rendering->Shutdown(m_services);
		if (m_uiOverlay)
		{
			m_uiOverlay->Shutdown(m_services);
		}
		if (m_cameras)
		{
			m_cameras->Shutdown();
		}
		m_services.Get<AssetSubsystem>().Shutdown();

		// profile (see the fullRuntime guard in Init); the UiShell launcher never
		if (m_animationBlend)
		{
			m_animationBlend->Shutdown();
		}

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
		m_services.Register<RenderThread>(m_renderThread);
		m_services.Register<IEngineRuntime>(static_cast<IEngineRuntime&>(*this));
	}

	void AetherCore::StopRenderThread()
	{
		// Join the render thread, THEN drain the GPU. RenderThread::Stop() does not
		m_renderThread.Stop();
		m_gpu->WaitIdle();
	}

	void AetherCore::RunExclusive(QuiesceMode mode, std::function<void()> mutation)
	{
		// main thread. A mutation that transitively re-enters would prematurely
		AE_ASSERT_ALWAYS(!m_renderThread.IsReloadInProgress(), "RunExclusive is non-reentrant");

		//     this cannot hang. Discard mode skips this: the render thread drops
		if (mode == QuiesceMode::Drain && m_producerFrameIndex > 0)
		{
			m_renderThread.WaitUntilFrameCompleted(m_producerFrameIndex - 1);
		}

		// (B) PARK the render thread outside ExecuteRenderFrame.
		m_renderThread.SetReloadInProgress(true);
		m_renderThread.WaitPaused();

		//     thread's identically-named join method.
		WaitIdle();

		// (D) Mutation runs single-threaded, GPU idle, render thread parked. Any
		if (mutation)
		{
			mutation();
		}

		m_renderThread.SetReloadInProgress(false);
	}

	int AetherCore::RunFrameLoop(EngineClient& client)
	{
		AE_PROFILE_ZONE();
		auto previousFrameTime = std::chrono::steady_clock::now();

		// endpoint and of screen-lock state (the ScreenshotService captures the
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
					AE_WARN(LogCategory::Engine, "Ignoring invalid AETHER_SCREENSHOT_FRAME value: {}", envFrame);
				}
			}
		}

		// Periodic frame-timing report, for measuring a build that has no Performance panel
		// - above all the shipped GameRuntime. Set AETHER_FRAME_REPORT to an interval in
		// seconds. Measuring the editor instead means measuring its ImGui, its extra
		// viewport presents, and Windows throttling it whenever it is not focused; none of
		// that is in the game, and all of it moves the numbers.
		double frameReportInterval = 0.0;
		if (const std::string env = readEnvironmentVariable("AETHER_FRAME_REPORT"); !env.empty())
		{
			try
			{
				frameReportInterval = std::stod(env);
			}
			catch (const std::exception&)
			{
				AE_WARN(LogCategory::Engine, "Ignoring invalid AETHER_FRAME_REPORT value: {}", env);
			}
		}
		auto lastFrameReport = std::chrono::steady_clock::now();
		std::vector<FrameTiming> reportFrames(FrameTimeline::kCapacity);
		std::vector<float> reportWall;
		reportWall.reserve(FrameTimeline::kCapacity);

		while (!ShouldClose())
		{
			AE_PROFILE_ZONE_N("Frame");

			const auto frameStart = std::chrono::steady_clock::now();

			m_framePacer.Wait();
			const auto afterPacer = std::chrono::steady_clock::now();

			client.OnFrameBegin();
			Logger::SetFrameNumber(m_producerFrameIndex);

			// rawDt is CLAMPED so a hitch cannot explode physics. wallSeconds is the same
			// interval unclamped, and is what the Performance panel reports - reporting the
			// clamped value made every frame worse than 30 fps look identical.
			constexpr double kMaxDeltaTime = 1.0 / 30.0;
			const auto now = std::chrono::steady_clock::now();
			const double wallSeconds = std::chrono::duration<double>(now - previousFrameTime).count();
			const double rawDt = std::min(wallSeconds, kMaxDeltaTime);
			previousFrameTime = now;

			PumpEvents();

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

			const auto beforeInFlightWait = std::chrono::steady_clock::now();
			if (m_producerFrameIndex >= Swapchain::kMaxFramesInFlight)
			{
				m_renderThread.WaitUntilFrameCompleted(m_producerFrameIndex - Swapchain::kMaxFramesInFlight);
			}
			const auto afterInFlightWait = std::chrono::steady_clock::now();

			Tick(static_cast<float>(rawDt));
			const double gameDt = rawDt * client.GetTimeScale();
			m_gameElapsedSeconds += gameDt;
			m_realElapsedSeconds += rawDt; // advances even when the game is paused (time scale 0)

			const auto drawSlot = static_cast<std::uint32_t>(m_producerFrameIndex % Swapchain::kMaxFramesInFlight);
			if (m_profile == RuntimeProfile::Full)
			{
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

			std::unique_ptr<IUiOverlayFrameData> overlayFrame;
			if (m_uiOverlay)
			{
				overlayFrame = m_uiOverlay->AcquireFrameData();
				m_uiOverlay->Render();

				// Structural viewport DESTROY (re-dock / close / toggle-off) must retire the
				const std::vector<std::uint32_t> departedViewports = m_uiOverlay->SecondaryViewportIdsWithPendingDestroy();
				if (!departedViewports.empty() || m_uiOverlay->HasPendingTextureUpdates())
				{
					// Release the ImGui frame lock BEFORE quiescing: RunExclusive drains and
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

			// which owns the ECS - never on the render thread, which runs concurrently
			if (m_rendering && m_profile == RuntimeProfile::Full)
			{
				ui::UiRenderer& uiRenderer = m_rendering->GetUiRenderer();
				uiRenderer.SetWorld(&m_services.Get<SceneSubsystem>().GetWorld());
				uiRenderer.SetCursorService(&m_cursor);
				uiRenderer.BuildFrame({static_cast<float>(packet.renderExtent.width), static_cast<float>(packet.renderExtent.height)}, packet.drawSlot);
			}

			m_renderThread.SubmitFrame(std::move(packet));

			if (!screenshotPath.empty() && !screenshotRequested && m_producerFrameIndex >= screenshotFrame && m_screenshotService.IsInitialized())
			{
				(void) m_screenshotService.Request(screenshotPath);
				screenshotRequested = true;
				AE_INFO(LogCategory::Engine, "Self-screenshot requested (frame {}) -> {}", m_producerFrameIndex, screenshotPath);
			}

			{
				const auto frameEnd = std::chrono::steady_clock::now();
				const auto ms = [](const auto a, const auto b)
				{
					return static_cast<float>(std::chrono::duration<double, std::milli>(b - a).count());
				};
				FrameTiming timing;
				timing.frameIndex = m_producerFrameIndex;
				timing.wallMs = static_cast<float>(wallSeconds * 1000.0);
				timing.simDtMs = static_cast<float>(rawDt * 1000.0);
				timing.pacerWaitMs = ms(frameStart, afterPacer);
				timing.inFlightWaitMs = ms(beforeInFlightWait, afterInFlightWait);
				// Everything the game thread did that was not spent waiting.
				timing.gameWorkMs = std::max(0.0f, ms(frameStart, frameEnd) - timing.pacerWaitMs - timing.inFlightWaitMs);
				m_frameTimeline.RecordGameFrame(timing);

				if (frameReportInterval > 0.0 && std::chrono::duration<double>(frameEnd - lastFrameReport).count() >= frameReportInterval)
				{
					lastFrameReport = frameEnd;
					const std::size_t count = m_frameTimeline.Snapshot(reportFrames);

					// Aggregated here rather than through ComputeFrameStats. That function is
					// shared with the editor's Performance panel, where it is exercised every
					// frame without incident, but calling it from THIS loop trips an /RTCs
					// stack-guard check in a Debug GameRuntime. Same Engine.lib, so the same
					// machine code passes 600 frames in EngineTests and fails here, which
					// points at this call site's environment rather than at the statistics.
					// That is still open (see the frame-report note in docs), and a
					// diagnostic must not be the thing that takes the process down.
					reportWall.clear();
					float gameWork = 0.0f;
					float inFlight = 0.0f;
					float present = 0.0f;
					std::size_t clamped = 0;
					for (std::size_t i = 0; i < count; ++i)
					{
						const FrameTiming& frame = reportFrames[i];
						reportWall.push_back(frame.wallMs);
						gameWork += frame.gameWorkMs;
						inFlight += frame.inFlightWaitMs;
						present += frame.presentWaitMs;
						if (frame.wallMs > frame.simDtMs + 0.01f)
						{
							++clamped;
						}
					}
					if (!reportWall.empty())
					{
						std::ranges::sort(reportWall);
						const auto n = static_cast<float>(reportWall.size());
						const auto at = [&](const double fraction)
						{
							const auto last = reportWall.size() - 1;
							return reportWall[std::min(static_cast<std::size_t>(std::llround(static_cast<double>(last) * fraction)), last)];
						};
						float total = 0.0f;
						for (const float value: reportWall)
						{
							total += value;
						}
						AE_INFO(LogCategory::Engine,
						        "FrameReport n={} avg={:.2f} median={:.2f} p95={:.2f} p99={:.2f} max={:.2f} | game={:.3f} inflight={:.3f} present={:.3f} | clamped={}",
						        count, total / n, at(0.5), at(0.95), at(0.99), reportWall.back(),
						        gameWork / n, inFlight / n, present / n, clamped);
					}
				}
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
		aether::Window::PollEvents();
	}

	void AetherCore::Tick(const float dt)
	{
		AE_PROFILE_ZONE();
		auto& platform = m_services.Get<PlatformSubsystem>();
		auto& input = platform.GetInput();
		input.Update();

		// Feed the engine cursor and keep the OS pointer in step with it. Done here, once, off the same
		// Input the game reads, so a project never has to remember to hide the real cursor when it turns
		// its own on - or to put it back when it turns it off.
		m_cursor.SetPosition(input.GetMousePos());
		// Suppression is not inferred here: only a host tool knows whether the game is actually presenting
		// (see ViewportPanel). A game running for real is never suppressed, which is the default.
		input.SetOsCursorVisible(m_cursor.WantsOsCursor());

		if (m_uiOverlay)
		{
			input.SetMouseCaptured(m_uiOverlay->WantsInputCapture() && !input.IsMouseViewportInputActive());
		}
		if (m_cameras)
		{
			m_cameras->GetCameraManager().Update(input, dt);
		}
	}

	void AetherCore::BeginFrame()
	{
		AE_PROFILE_ZONE();
		// the producer (main) thread via NeedsSwapchainOrViewportRecreate() +
		m_gpu->BeginSwapchainFrame();
		m_currentCmdList = gpu::CommandList(m_gpu->GetSwapchain().GetCurrentCommandBuffer());
	}

	bool AetherCore::NeedsSwapchainOrViewportRecreate()
	{
		const bool framebufferResized = m_services.Get<PlatformSubsystem>().GetWindow().PeekFramebufferResized();
		const bool swapchainOutOfDate = m_gpu->SwapchainNeedsRecreation();
		const bool viewportPending = m_rendering != nullptr && m_rendering->IsSceneViewportRebuildPending();
		// A lazily-allocated target wants creating or releasing. The rebuild rides the
		// same quiesced path as a viewport change: render thread parked, GPU idle, graph
		// rebuilt around what exists afterwards.
		const bool lazyTargetsPending = m_rendering != nullptr && m_rendering->IsLazyTargetRebuildPending();
		return framebufferResized || swapchainOutOfDate || viewportPending || lazyTargetsPending;
	}

	void AetherCore::RecreateSwapchainAndResources()
	{
		const bool framebufferResized = m_services.Get<PlatformSubsystem>().GetWindow().ConsumeFramebufferResized();
		const bool swapchainDirty = framebufferResized || m_gpu->SwapchainNeedsRecreation();

		const bool viewportCommitted = m_rendering != nullptr && m_rendering->CommitPendingSceneViewportSettings();
		const bool lazyTargetsCommitted = m_rendering != nullptr && m_rendering->CommitPendingLazyTargets();

		if (swapchainDirty)
		{
			RecreateSwapchain();
		}
		else if ((viewportCommitted || lazyTargetsCommitted) && m_rendering != nullptr)
		{
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
			// recreate makes the producer thread pick up the new present mode.
			m_gpu->RequestSwapchainRecreation();
		}
		AE_INFO(LogCategory::Engine, "VSync {}.", enabled ? "enabled" : "disabled");
	}

	void AetherCore::SetImguiViewportsEnabled(bool enabled)
	{
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
		const MaterialBuffer& materialBuffer = assetsSub.GetMaterialBuffer();
		const EffectParamBuffer& effectParamBuffer = assetsSub.GetEffectParamBuffer();
		CameraManager& cameras = m_cameras->GetCameraManager();
		const Renderer& renderer = m_rendering->GetRenderer();

		// Collision-only view: leave the queue and 2D packet empty so nothing
		// but the clear colour and the collider wireframes reaches the screen.
		const bool collisionOnly = IsCollisionOnlyViewEnabled();

		renderQueue.SetWriteSlot(drawSlot);
		if (!collisionOnly)
		{
			WorldRenderer::Flush(world, renderQueue);
		}

		rttService.PrepareQueues(drawSlot, world);

		const glm::vec4 sunDirIntensity = renderer.GetDirectionalLightVector();
		const bool directionalShadowEnabled = sunDirIntensity.w > 0.001f;
		shadowService.SetDirectionalShadowEnabled(directionalShadowEnabled);
		// What the world actually submitted this frame, measured after the flush. This is
		// what the shadow, AO and preview targets are allocated against - never a scene
		// feature flag, which every scene carries whether or not it has the content.
		const bool directionalShadowCasters = shadowService.PrepareQueues(drawSlot, world);
		const bool localShadowCasters = localShadowService.PrepareQueues(drawSlot, world);
		m_rendering->GetCameraPreview().PrepareQueue(drawSlot, world);
		m_rendering->GetModelPreview().PrepareQueue(drawSlot);

		RenderFramePacket packet;
		packet.frameIndex = frameIndex;
		packet.drawSlot = drawSlot;
		packet.renderExtent = extent;
		packet.materialBufferAddr = materialBuffer.GetDeviceAddressU64();
		packet.effectParamBufferAddr = effectParamBuffer.GetDeviceAddressU64();
		packet.sceneFeatures = world.GetSceneFeatures();
		if (!collisionOnly)
		{
			assetsSub.GetSpriteSystem().Extract(world, packet.render2D);
		}

		View2DBounds tileView; // invalid = no chunk culling (non-ortho views)
		if (const Camera* cam = cameras.TryGetMainCamera())
		{
			packet.hasCameraData = true;
			packet.view = cam->GetViewMatrix();
			const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
			packet.proj = cam->GetProjectionMatrix(aspect);
			packet.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
			packet.cameraNearPlane = cam->GetNearPlane();
			if (cam->GetProjection() == CameraProjection::Orthographic)
			{
				const glm::vec2 halfExtent{cam->GetOrthographicHeight() * aspect * 0.5f, cam->GetOrthographicHeight() * 0.5f};
				tileView.min = glm::vec2(cam->GetPosition()) - halfExtent;
				tileView.max = glm::vec2(cam->GetPosition()) + halfExtent;
				tileView.valid = true;
			}
		}
		// Tiles append into the same instance stream as sprites; one shared sort
		// interleaves them by sort key.
		if (!collisionOnly)
		{
			assetsSub.GetTileMapSystem().Extract(world, tileView, static_cast<float>(m_gameElapsedSeconds), packet.render2D);
		}
		// Live particles append into the same 2D instance stream.
		if (!collisionOnly)
		{
			if (auto* particles = static_cast<ParticleSystem*>(world.FindSystem("ParticleSystem")))
			{
				particles->Extract(world, packet.render2D);
			}
		}
		Finalize2DFrame(packet.render2D);

		// Project custom passes: scripts submit their pass buffers into m_customPasses during the
		// update; hand this frame's submissions to the render packet, then clear for the next frame.
		if (!collisionOnly)
		{
			packet.renderCustom = std::move(m_customPasses.frame);
			m_customPasses.frame.passes.clear();
		}

		packet.sunDirectionIntensity = sunDirIntensity;
		packet.directionalShadowEnabled = directionalShadowEnabled;
		packet.ambientColor = renderer.GetAmbientLightVector();

		// 2D light-map settings: default to the shared ambient + engine shadow defaults; the first
		// Light2DSettingsComponent in the scene (if any) overrides for authored per-scene mood.
		packet.light2DAmbient = packet.ambientColor;
		packet.light2DShadowParams = glm::vec4(0.94f, 1.0f, 0.0f, 0.0f);
		for (const auto e: world.GetRegistry().view<Light2DSettingsComponent>())
		{
			const auto& s = world.GetRegistry().get<Light2DSettingsComponent>(e);
			packet.light2DAmbient = glm::vec4(s.ambientColor * s.ambientIntensity, 1.0f);
			packet.light2DShadowParams = glm::vec4(s.shadowStrength, s.shadowSoftness, 0.0f, 0.0f);
			packet.light2DHasSettings = true;
			break;
		}

		packet.sunColor = renderer.GetSunColorVector();
		packet.skyHorizonColor = renderer.GetSkyHorizonColorVector();
		packet.skyZenithColor = renderer.GetSkyZenithColorVector();
		packet.skyVoidColor = renderer.GetSkyVoidColorVector();

		// The scene background is owned by the main camera. Solid/Gradient are
		// composited WYSIWYG in the tonemap pass (see PostProcessStack); only the
		// SkyGradient mode uses the procedural sky above.
		if (const Entity mainCamera = ecs::GetMainCameraEntity(world); mainCamera.IsValid())
		{
			if (const auto* cam = world.TryGet<CameraComponent>(mainCamera); cam != nullptr)
			{
				packet.backgroundMode = static_cast<std::uint32_t>(cam->background);
				packet.backgroundAngleRadians = glm::radians(cam->gradientAngleDegrees);
				packet.backgroundStopCount = 0;
				// skyVoidColor.w carries the sky-vs-WYSIWYG flag for the skybox
				// pass: >= 0.5 draws the procedural sky, < 0.5 emits transparent
				// coverage so the tonemap compositor fills those pixels.
				packet.skyVoidColor.w = (cam->background == CameraBackground::SkyGradient) ? 1.0f : 0.0f;
				if (cam->background == CameraBackground::SolidColour)
				{
					packet.backgroundStopCount = 1;
					packet.backgroundStops[0] = glm::vec4(cam->clearColor, 0.0f);
				}
				else if (cam->background == CameraBackground::Gradient)
				{
					const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(cam->gradientStops.size()), RenderFramePacket::kMaxBackgroundStops);
					packet.backgroundStopCount = n;
					for (std::uint32_t i = 0; i < n; ++i)
					{
						packet.backgroundStops[i] = glm::vec4(cam->gradientStops[i].colour, cam->gradientStops[i].position);
					}
				}
			}
		}

		packet.pointLights.assign(renderer.GetPointLights().begin(), renderer.GetPointLights().end());

		// Script-submitted transient 2D lights/occluders (a drawn ink stroke, an effect): appended after
		// the ECS lights so they light and cast shadows like any other, then cleared for the next frame.
		// A script that stops submitting simply stops lighting - no entity lifetime to manage.
		if (!m_light2DSubmissions.lights.empty())
		{
			packet.pointLights.insert(packet.pointLights.end(), m_light2DSubmissions.lights.begin(), m_light2DSubmissions.lights.end());
		}
		if (!m_light2DSubmissions.occluders.empty())
		{
			packet.render2D.occluders.insert(packet.render2D.occluders.end(), m_light2DSubmissions.occluders.begin(), m_light2DSubmissions.occluders.end());
		}
		m_light2DSubmissions.Clear();

		// Hand the game-thread debug vertex buffer to the packet. This is the
		packet.debugVertices = std::move(m_pendingDebugVertices);
		m_pendingDebugVertices.clear();
		packet.spotLights.assign(renderer.GetSpotLights().begin(), renderer.GetSpotLights().end());

		// presence-or-absence. The render thread reads neither global.
		packet.debugRenderingEnabled = IsDebugRenderingEnabled() || collisionOnly;
		m_rendering->GetPhysicsDebugRenderer().ExtractShapes(world, packet.physicsDebugShapes);
		// 2D collider wireframes ride the immediate-mode line channel, gated by
		// the same physics-debug toggle as the 3D shapes.
		if (IsPhysicsDebugShapesEnabled() || collisionOnly)
		{
			ExtractPhysics2DDebugLines(world, packet.debugVertices);
		}

		packet.hasSceneDraws = !renderQueue.IsEmpty(drawSlot);

		const auto anyCastsShadow = [](const auto& lights) { return std::ranges::any_of(lights, [](const auto& light) { return light.castsShadow; }); };
		m_rendering->PublishContentSignals(RenderContentSignals{
		        .sceneDraws = packet.hasSceneDraws,
		        .shadowCasterDraws = directionalShadowCasters || localShadowCasters,
		        .directionalLight = directionalShadowEnabled,
		        .localShadowLights = anyCastsShadow(packet.pointLights) || anyCastsShadow(packet.spotLights),
		        .texturePreview = m_rendering->IsTexturePreviewRequested(),
		        // Asked after every queue for this slot has been filled - the main flush, the
		        // two shadow services, both previews and the render-to-texture targets above.
		        .skinnedDraws = m_rendering->HasSkinnedDrawsQueued(drawSlot),
		});

		return packet;
	}

	void AetherCore::ExecuteRenderFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		const auto execStart = std::chrono::steady_clock::now();
		m_frameIndex = packet.frameIndex;
		const auto acquireStart = std::chrono::steady_clock::now();
		BeginFrame();
		const auto acquireEnd = std::chrono::steady_clock::now();

		// INVARIANT: the render thread reads ONLY `packet`, never the live ECS. All
		if (m_rendering)
		{
			m_rendering->SetSceneFeatures(packet.sceneFeatures);
			m_rendering->SetFrameSceneDraws(packet.hasSceneDraws);
			m_rendering->SetBackgroundParams(packet.backgroundMode, packet.backgroundAngleRadians, packet.backgroundStopCount, packet.backgroundStops);
			PhysicsDebugRenderer& debugRenderer = m_rendering->GetPhysicsDebugRenderer();
			debugRenderer.SetFrameDebugVertices(&packet.debugVertices);
			debugRenderer.SetFramePhysicsShapes(&packet.physicsDebugShapes);
			debugRenderer.SetFrameDebugEnabled(packet.debugRenderingEnabled);
			if (m_profile == RuntimeProfile::Full)
			{
				m_rendering->GetRenderer2D().BeginFrame(packet.render2D, packet.drawSlot);
				m_rendering->GetCustomPassRenderer().BeginFrame(packet.renderCustom, packet.drawSlot);
				m_rendering->GetLight2DCompositor().BeginFrame(packet, packet.drawSlot);
			}
		}

		const auto presentStart = std::chrono::steady_clock::now();
		EndFrame(packet);
		const auto presentEnd = std::chrono::steady_clock::now();
		if (m_rendering && m_profile == RuntimeProfile::Full)
		{
			m_rendering->GetRenderer2D().EndFrame();
			m_rendering->GetCustomPassRenderer().EndFrame();
		}

		// (render thread owns the queue; the readback is self-contained).
		if (m_screenshotService.IsInitialized())
		{
			m_screenshotService.ProcessPending(static_cast<void*>(m_gpu->GetSwapchain().GetCurrentImage()), m_gpu->GetSwapchainExtent(), m_gpu->GetSwapchainColorFormat());
		}

		AE_PROFILE_PLOT("Frame/RenderThreadExecNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - execStart).count()));

		{
			const auto ms = [](const auto a, const auto b)
			{
				return static_cast<float>(std::chrono::duration<double, std::milli>(b - a).count());
			};
			const auto execEnd = std::chrono::steady_clock::now();
			// presentWait is both places the render thread can block on the swapchain:
			// acquiring an image, and submitting/presenting it. With FIFO vsync this is
			// where the refresh cadence actually enters the frame.
			const float presentWaitMs = ms(acquireStart, acquireEnd) + ms(presentStart, presentEnd);
			// Report render EXEC with the present block taken out. The two are nested, and a
			// phase breakdown whose rows overlap cannot be read as a breakdown - it would
			// show the same blocked refresh interval twice and never sum to a frame.
			const float renderWorkMs = std::max(0.0f, ms(execStart, execEnd) - presentWaitMs);
			m_frameTimeline.RecordRenderFrame(packet.frameIndex, renderWorkMs, presentWaitMs);
		}
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

		FrameConstants fc = aether::GpuDevice::ComposeBaseFrameConstants(packet, glm::mat4(1.0f));

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
			aether::GpuDevice::ApplyNoCameraLightingFallback(fc);
		}

		UploadFrameConstantsAndExecuteRenderGraph(frameIdx, fc);
		if (m_uiOverlay && packet.uiOverlay)
		{
			m_uiOverlay->RenderFrame(*packet.uiOverlay, m_currentCmdList, m_gpu->BuildFrameTarget());
		}

		SubmitAndAdvance(frameIdx);

		// Secondary (torn-out) viewports render + present on the render thread, after the
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
				aether::GpuDevice::ApplyNoCameraLightingFallback(fc);
			}

			lightingMgr.UpdateBufferHandles(m_rendering->GetRenderGraph(), frameIdx);
		}
		else
		{
			aether::GpuDevice::ApplyNoCameraLightingFallback(fc);
		}
	}

	void AetherCore::PatchShadowIndices(const std::uint32_t frameIdx)
	{
		m_cameras->GetLightingManager().ApplyShadowIndices(frameIdx, m_rendering->GetLocalShadowService().GetLightShadowIndices());
	}

	void AetherCore::UploadFrameConstantsAndExecuteRenderGraph(std::uint32_t frameIdx, FrameConstants fc)
	{
		const bool fullRuntime = m_profile == RuntimeProfile::Full;

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

		renderGraph.SubmitComputeWork(frameIdx);

		const gpu::TimelineSemaphoreHandle graphAsyncSem = renderGraph.HasAsyncComputeWork() ? renderGraph.GetComputeTimelineSemaphore() : nullptr;
		const std::uint64_t graphAsyncVal = renderGraph.HasAsyncComputeWork() ? renderGraph.GetComputeTimelineValue() : 0;

		m_gpu->SubmitAndPresent(graphAsyncSem, graphAsyncVal);

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
