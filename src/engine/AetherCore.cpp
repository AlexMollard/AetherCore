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
#include "imgui/ImguiSubsystem.hpp"
#include "io/FileSystem.hpp"
#include "material/MaterialBuffer.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderingSubsystem.hpp"
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
		m_settings = settings;
		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		// Create all subsystems.
		m_services.Register<AetherCore>(*this);
		m_services.RegisterOwned(std::make_unique<PlatformSubsystem>());
		m_services.RegisterOwned(std::make_unique<SceneSubsystem>());
		m_services.RegisterOwned(std::make_unique<AssetSubsystem>());
		m_services.RegisterOwned(std::make_unique<AsyncComputeContext>());
		m_gpu = std::make_unique<GpuDevice>();
		m_services.Register<GpuDevice>(*m_gpu);
		m_cameras = std::make_unique<CameraSubsystem>();
		m_imgui = std::make_unique<ImguiSubsystem>();
		m_rendering = std::make_unique<RenderingSubsystem>();

		auto& platform = m_services.Get<PlatformSubsystem>();
		auto& sceneSub = m_services.Get<SceneSubsystem>();
		auto& assetsSub = m_services.Get<AssetSubsystem>();

		// -- 1. Platform -----------------------------------------------------
		platform.Init({.appName = config.appName, .width = config.width, .height = config.height});
		m_services.Register<Window>(platform.GetWindow());
		m_services.Register<Input>(platform.GetInput());

		// -- 2. Graphics device ----------------------------------------------
		AE_EXPECT_OR_THROW_VOID(m_gpu->Init(m_services, {.appName = config.appName, .enableVsync = config.enableVsync}));

		// -- 3. Scene (ECS + legacy) -----------------------------------------
		sceneSub.Init();
		m_services.Register<World>(sceneSub.GetWorld());

		// -- 4. Assets -------------------------------------------------------
		assetsSub.Init(m_services);
		m_services.Register<AssetManager>(assetsSub.GetAssetManager());
		m_services.Register<MeshArena>(assetsSub.GetMeshArena());
		m_services.Register<MeshUploadQueue>(assetsSub.GetMeshUploadQueue());
		m_services.Register<MaterialBuffer>(assetsSub.GetMaterialBuffer());
		m_services.Register<AssetSubsystem>(assetsSub);

		// -- 5. Cameras ------------------------------------------------------
		m_cameras->Init(m_services);
		m_services.Register<CameraManager>(m_cameras->GetCameraManager());
		m_services.Register<LightingManager>(m_cameras->GetLightingManager());

		// -- 6. Rendering ----------------------------------------------------
		m_rendering->Init(m_services);
		m_rendering->SetFrameIndexProvider([this]() { return m_frameIndex; });
		m_services.Register<RenderingSubsystem>(*m_rendering);
		m_services.Register<Renderer>(m_rendering->GetRenderer());
		m_services.Register<RenderQueue>(m_rendering->GetRenderQueue());
		m_services.Register<RenderGraph>(m_rendering->GetRenderGraph());
		m_services.Register<ShadowService>(m_rendering->GetShadowService());
		m_services.Register<RenderTargetService>(m_rendering->GetRenderTargetService());

		// -- 7. ImGui tooling -----------------------------------------------
		m_imgui->Init(m_services);
		m_services.Register<ImguiSubsystem>(*m_imgui);

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
		m_rendering->RegisterPasses(m_services);

		// -- 11. Animation systems -------------------------------------------
		m_animationBlend = std::make_unique<AnimationBlendSystem>();

		m_animationBlend->Init(256, 128);

		m_services.Register<AnimationBlendSystem>(*m_animationBlend);

		RenderQueue& rq = m_rendering->GetRenderQueue();
		rq.SetAnimationBlendSystem(m_animationBlend.get());

		// -- 10. Swapchain recreation callback ------------------------------
		m_gpu->SetSwapchainRecreatedCallback([this]() { m_rendering->RecreateSwapchainResources(m_services); });

		AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless sampled-image capacity: {}", m_gpu->GetBindlessManager().GetCapacity());
	}

	AetherCore::~AetherCore()
	{
		m_gpu->WaitIdle();

		m_services.Get<AsyncComputeContext>().Shutdown(*m_gpu);

		// Subsystems free their VMA-backed allocations (VMA still alive).
		m_rendering->Shutdown();
		m_imgui->Shutdown(m_services);
		m_cameras->Shutdown();
		m_services.Get<AssetSubsystem>().Shutdown();
		// SceneSubsystem has no shutdown work.

		// Animation systems (reverse of init order).
		m_animationBlend->Shutdown();

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
		if (m_imgui)
		{
			input.SetMouseCaptured(m_imgui->WantsInputCapture() && !input.IsMouseViewportInputActive());
		}
		m_cameras->GetCameraManager().Update(input, dt);
	}

	void AetherCore::BeginFrame()
	{
		AE_PROFILE_ZONE();
		if (m_gpu->SwapchainNeedsRecreation())
		{
			if (m_rendering)
			{
				m_rendering->CommitPendingSceneViewportSettings();
			}
			RecreateSwapchain();
		}
		else if (m_rendering)
		{
			m_rendering->ApplyPendingSceneViewportChanges(m_services);
		}

		m_gpu->BeginSwapchainFrame();
		m_currentCmdList = gpu::CommandList(m_gpu->GetSwapchain().GetCurrentCommandBuffer());
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

		RenderFramePacket packet;
		packet.frameIndex = frameIndex;
		packet.drawSlot = drawSlot;
		packet.renderExtent = extent;
		packet.materialBufferAddr = materialBuffer.GetDeviceAddressU64();

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

		return packet;
	}

	void AetherCore::ExecuteRenderFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		const auto execStart = std::chrono::steady_clock::now();
		m_frameIndex = packet.frameIndex;
		BeginFrame();

		if (m_rendering)
		{
			World& world = m_services.Get<SceneSubsystem>().GetWorld();
			PhysicsDebugRenderer& debugRenderer = m_rendering->GetPhysicsDebugRenderer();
			debugRenderer.SetFrameDebugVertices(&packet.debugVertices);
			debugRenderer.SetWorld(&world);
		}

		EndFrame(packet);
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
			m_gpu->AdvanceResourceRegistryFrame();
			return;
		}

		FrameConstants fc = m_gpu->ComposeBaseFrameConstants(packet, glm::mat4(1.0f));

		BuildShadowsAndRunLighting(packet, frameIdx, fc);

		if (packet.hasCameraData)
		{
			PatchShadowIndices(frameIdx);
		}

		UploadFrameConstantsAndExecuteRenderGraph(frameIdx, fc);
		m_imgui->RenderFrame(packet.imgui, m_currentCmdList, m_gpu->BuildFrameTarget());

		SubmitAndAdvance(frameIdx);
	}

	void AetherCore::BuildShadowsAndRunLighting(const RenderFramePacket& packet, std::uint32_t frameIdx, FrameConstants& fc)
	{
		m_rendering->GetShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras->GetCameraManager(), fc);
		m_rendering->GetLocalShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras->GetCameraManager(), m_services.Get<SceneSubsystem>().GetWorld(), fc);

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
		m_gpu->AdvanceResourceRegistryFrame();
	}

	GpuFormat AetherCore::GetForwardColorFormat()
	{
		return GpuDevice::GetForwardColorFormat();
	}

} // namespace aether
