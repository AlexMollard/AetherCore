#include "AetherCore.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/GltfAsset.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/World.hpp"
#include "io/FileSystem.hpp"
#include "gpu/BindlessManager.hpp"
#include "material/MaterialBuffer.hpp"
#include "rendering/RenderThread.hpp"
#include "utils/GpuProfiler.hpp"
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
		m_settings = settings;
		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		// ── 1. Platform ─────────────────────────────────────────────────────
		m_platform.Init({.appName = config.appName, .width = config.width, .height = config.height});
		m_services.Register<Window>(m_platform.GetWindow());
		m_services.Register<Input>(m_platform.GetInput());

		// ── 2. Graphics device ──────────────────────────────────────────────
		m_gpu.Init(m_services, {.appName = config.appName, .enableVsync = config.enableVsync});

		// ── 3. Scene (ECS + legacy) ─────────────────────────────────────────
		m_sceneSub.Init();
		m_services.Register<World>(m_sceneSub.GetWorld());
		m_services.Register<Scene>(m_sceneSub.GetScene());

		// ── 4. Assets ───────────────────────────────────────────────────────
		m_assetsSub.Init(m_services);
		m_services.Register<AssetManager>(m_assetsSub.GetAssetManager());
		m_services.Register<MeshArena>(m_assetsSub.GetMeshArena());
		m_services.Register<MeshUploadQueue>(m_assetsSub.GetMeshUploadQueue());
		m_services.Register<MaterialBuffer>(m_assetsSub.GetMaterialBuffer());
		m_services.Register<AssetSubsystem>(m_assetsSub);

		// ── 5. Cameras ──────────────────────────────────────────────────────
		m_cameras.Init(m_services);
		m_services.Register<CameraManager>(m_cameras.GetCameraManager());
		m_services.Register<LightingManager>(m_cameras.GetLightingManager());

		// ── 6. Rendering ────────────────────────────────────────────────────
		m_rendering.Init(m_services);
		m_rendering.SetFrameIndexProvider([this]() { return m_frameIndex; });
		m_services.Register<Renderer>(m_rendering.GetRenderer());
		m_services.Register<RenderQueue>(m_rendering.GetRenderQueue());
		m_services.Register<RenderGraph>(m_rendering.GetRenderGraph());
		m_services.Register<ShadowService>(m_rendering.GetShadowService());
		m_services.Register<RenderTargetService>(m_rendering.GetRenderTargetService());

		// ── 7. UI ──────────────────────────────────────────────────────────
		if (config.uiFontPath != nullptr && config.uiFontPath[0] != '\0')
		{
			m_ui.Init(m_services, config.uiFontPath, config.uiPassNamePrefix, config.uiGlyphSize);
			m_services.Register<UIRenderer>(m_ui.GetUiRenderer());
			m_services.Register<ui::UiContext>(m_ui.GetUiContext());
			m_services.Register<ui::UiSystem>(m_ui.GetUiSystem());
		}

		// Link cross-subsystem dependencies.
		m_cameras.GetLightingManager().LinkRenderer(m_rendering.GetRenderer());
		m_assetsSub.LinkRenderingDeps(m_services);

		// ── 8. Create default main camera ───────────────────────────────────
		CameraManager& cameras = m_services.Get<CameraManager>();
		const CameraHandle mainCam = cameras.Create(CameraDesc{});
		cameras.SetMainCamera(mainCam);

		// ── 9. Async compute (optional) ─────────────────────────────────────
		bool enableAsyncCompute = m_settings.graphics.asyncCompute && m_gpu.HasDedicatedComputeQueue();
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
			m_asyncCompute.Init(m_gpu);
		}

		// ── 10. Swapchain recreation callback ──────────────────────────────
		m_gpu.SetSwapchainRecreatedCallback(
		        [this]()
		        {
			        m_rendering.RecreateSwapchainResources(m_services);
			        // UI render graph passes are cleared by the reset above;
			        // eagerly re-register them so the lazy-check is skipped on
			        // every subsequent Draw* call.
			        if (auto* ui = m_services.TryGet<UIRenderer>())
			        {
				        ui->ReRegisterPass();
			        }
		        });

		AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless sampled-image capacity: {}", m_gpu.GetBindlessManager().GetCapacity());
	}

	AetherCore::~AetherCore()
	{
		m_gpu.WaitIdle();

		m_asyncCompute.Shutdown(m_gpu);

		// Subsystems free their VMA-backed allocations (VMA still alive).
		m_rendering.Shutdown(m_services);
		m_ui.Shutdown(m_services);
		m_cameras.Shutdown();
		m_assetsSub.Shutdown();
		// SceneSubsystem has no shutdown work.

		// GPU shutdown destroys internal Vulkan resources.
		m_gpu.Shutdown();
		m_platform.Shutdown();

		m_services.Clear();
		io::FileSystem::Shutdown();
	}

	void AetherCore::WaitIdle()
	{
		m_gpu.WaitIdle();
	}

	bool AetherCore::ShouldClose()
	{
		return m_platform.GetWindow().ShouldClose();
	}

	void AetherCore::PumpEvents()
	{
		m_platform.GetWindow().PollEvents();
	}

	void AetherCore::Tick(const float dt)
	{
		AE_PROFILE_ZONE();
		m_platform.GetInput().Update();
		m_cameras.GetCameraManager().Update(m_platform.GetInput(), dt);
	}

	void AetherCore::BeginFrame()
	{
		AE_PROFILE_ZONE();
		if (m_gpu.SwapchainNeedsRecreation())
		{
			RecreateSwapchain();
		}

		m_gpu.BeginSwapchainFrame();
		m_currentRecorder = m_gpu.GetCurrentCommandRecorder();
	}

	void AetherCore::RecreateSwapchain()
	{
		auto size = m_platform.GetWindow().WaitForValidFramebufferSize();
		m_gpu.RecreateSwapchain(m_platform.GetWindow(), m_settings.graphics.vsync);

		AE_INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", size.width, size.height);
	}

	RenderFramePacket AetherCore::PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		GpuExtent2D extent = m_gpu.GetSwapchainExtent();
		RenderQueue& renderQueue = m_rendering.GetRenderQueue();
		Scene& scene = m_sceneSub.GetScene();
		World& world = m_sceneSub.GetWorld();
		RenderTargetService& rttService = m_rendering.GetRenderTargetService();
		ShadowService& shadowService = m_rendering.GetShadowService();
		MaterialBuffer& materialBuffer = m_assetsSub.GetMaterialBuffer();
		CameraManager& cameras = m_cameras.GetCameraManager();
		Renderer& renderer = m_rendering.GetRenderer();

		renderQueue.SetWriteSlot(drawSlot);
		WorldRenderer::Flush(scene, renderQueue);
		WorldRenderer::Flush(world, renderQueue);

		rttService.PrepareQueues(drawSlot, scene, world);
		shadowService.PrepareQueues(drawSlot, scene, world);

		RenderFramePacket packet;
		packet.frameIndex = frameIndex;
		packet.drawSlot = drawSlot;
		packet.materialBufferAddr = materialBuffer.GetDeviceAddressU64();

		if (const Camera* cam = cameras.TryGetMainCamera())
		{
			packet.hasCameraData = true;
			packet.view = cam->GetViewMatrix();
			const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
			packet.proj = cam->GetProjectionMatrix(aspect);
			packet.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
		}

		packet.sunDirectionIntensity = renderer.GetDirectionalLightVector();
		packet.ambientColor = renderer.GetAmbientLightVector();
		packet.sunColor = renderer.GetSunColorVector();
		packet.skyHorizonColor = renderer.GetSkyHorizonColorVector();
		packet.skyZenithColor = renderer.GetSkyZenithColorVector();
		packet.skyVoidColor = renderer.GetSkyVoidColorVector();

		packet.pointLights.assign(renderer.GetPointLights().begin(), renderer.GetPointLights().end());
		packet.spotLights.assign(renderer.GetSpotLights().begin(), renderer.GetSpotLights().end());

		return packet;
	}

	void AetherCore::ExecuteRenderFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		const auto execStart = std::chrono::steady_clock::now();
		m_frameIndex = packet.frameIndex;
		BeginFrame();
		EndFrame(packet);
		AE_PROFILE_PLOT("Frame/RenderThreadExecNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - execStart).count()));
	}

	void AetherCore::EndFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();

		if (!m_gpu.IsSwapchainFrameValid())
		{
			m_gpu.SubmitAndPresent();
			++m_frameIndex;
			m_gpu.GetBindlessManager().AdvanceFrame(m_frameIndex);
			return;
		}

		const auto frameIdx = static_cast<std::uint32_t>(packet.frameIndex % kMaxFramesInFlight);

		FrameConstants fc = m_rendering.GetFrameComposer().ComposeBaseFrameConstants(packet, m_sceneSub.GetScene().GetViewProjection());

		BuildShadowsAndRunLighting(packet, frameIdx, fc);

		if (packet.hasCameraData)
		{
			PatchShadowIndices(frameIdx);
		}

		UploadFrameConstantsAndExecuteRenderGraph(frameIdx, fc);

		SubmitAndAdvance();
	}

	void AetherCore::BuildShadowsAndRunLighting(const RenderFramePacket& packet, std::uint32_t frameIdx, FrameConstants& fc)
	{
		m_rendering.GetShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras.GetCameraManager(), fc);
		m_rendering.GetLocalShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras.GetCameraManager(), m_sceneSub.GetScene(), m_sceneSub.GetWorld(), fc);

		if (packet.hasCameraData)
		{
			const Camera* cam = m_cameras.GetCameraManager().TryGetMainCamera();
			if (cam)
			{
				if (m_asyncCompute.IsEnabled())
				{
					m_asyncCompute.BeginFrame(m_gpu, frameIdx);
					CommandRecorder lightingCmd = m_asyncCompute.GetCommandRecorder(frameIdx);
					m_cameras.GetLightingManager().UpdateForView(frameIdx, lightingCmd, *cam, m_gpu.GetSwapchainExtent(), fc, true, /*isAsyncCompute=*/true, packet.pointLights, packet.spotLights);
					m_asyncCompute.EndCommandBuffer(frameIdx);
					m_asyncComputeSubmitResult = m_asyncCompute.Submit(m_gpu, frameIdx);
				}
				else
				{
					const TracyVkCtx vkCtx = m_gpu.GetVulkanContext().GetTracyVkCtx();
					AE_PROFILE_GPU_ZONE_T(vkCtx, m_currentRecorder.GetCommandBuffer(), gpuLightingZone, "Lighting.UpdateForView");
					m_cameras.GetLightingManager().UpdateForView(frameIdx, m_currentRecorder, *cam, m_gpu.GetSwapchainExtent(), fc, true, /*isAsyncCompute=*/false, packet.pointLights, packet.spotLights);
				}
			}
		}
		else
		{
			m_rendering.GetFrameComposer().ApplyNoCameraLightingFallback(fc);
		}
	}

	void AetherCore::PatchShadowIndices(const std::uint32_t frameIdx)
	{
		m_cameras.GetLightingManager().ApplyShadowIndices(frameIdx, m_rendering.GetLocalShadowService().GetLightShadowIndices());
	}

	void AetherCore::UploadFrameConstantsAndExecuteRenderGraph(std::uint32_t frameIdx, const FrameConstants& fc)
	{
		m_rendering.GetFrameConstantsBuffer().Write(frameIdx, fc);
		const std::uint64_t frameAddr = m_rendering.GetFrameConstantsBuffer().GetDeviceAddressU64(frameIdx);

		m_currentRecorder.HostToShaderBarrier();

		const FrameTarget frameTarget = m_gpu.BuildFrameTarget();

		m_currentRecorder.BeginDebugLabel("Frame.RenderGraph", 0.35f, 0.55f, 0.95f, 1.0f);
		m_rendering.GetRenderGraph().BeginFrame(frameIdx);
		m_rendering.GetRenderGraph().Execute(m_currentRecorder, frameTarget, frameAddr, frameIdx);
		m_currentRecorder.EndDebugLabel();
	}

	void AetherCore::SubmitAndAdvance()
	{
		if (m_asyncCompute.IsEnabled())
		{
			m_gpu.SubmitAndPresent(m_asyncComputeSubmitResult.semaphoreHandle, m_asyncComputeSubmitResult.timelineValue);
		}
		else
		{
			m_gpu.SubmitAndPresent();
		}

		++m_frameIndex;
		m_gpu.GetBindlessManager().AdvanceFrame(m_frameIndex);
	}

} // namespace aether
