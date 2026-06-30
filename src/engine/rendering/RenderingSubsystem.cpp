#include "rendering/RenderingSubsystem.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/LightingManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/MaterialBuffer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/DiagnosticEngine.hpp"

namespace
{
	constexpr std::uint32_t kRenderQueueMaxDraws = 65536;
} // namespace

namespace aether
{
	gpu::Extent2D RenderingSubsystem::ResolveSceneViewportExtent(gpu::Extent2D swapchainExtent) const
	{
		if (!m_sceneViewportEnabled)
		{
			return swapchainExtent;
		}

		auto clampExtent = [](gpu::Extent2D extent)
		{
			extent.width = std::clamp(extent.width, 64u, 8192u);
			extent.height = std::clamp(extent.height, 64u, 8192u);
			return extent;
		};

		switch (m_sceneViewportSettings.resolutionMode)
		{
			case SceneViewportResolutionMode::Fixed720p:
				return {1280, 720};
			case SceneViewportResolutionMode::Fixed1080p:
				return {1920, 1080};
			case SceneViewportResolutionMode::Fixed1440p:
				return {2560, 1440};
			case SceneViewportResolutionMode::Custom:
				return clampExtent(m_sceneViewportSettings.customExtent);
			case SceneViewportResolutionMode::WindowNative:
			default:
				return clampExtent(swapchainExtent);
		}
	}

	gpu::Extent2D RenderingSubsystem::ResolveRequestedSceneViewportExtent(gpu::Extent2D swapchainExtent) const
	{
		auto clampExtent = [](gpu::Extent2D extent)
		{
			extent.width = std::clamp(extent.width, 64u, 8192u);
			extent.height = std::clamp(extent.height, 64u, 8192u);
			return extent;
		};

		SceneViewportSettings settings;
		bool enabled = false;
		{
			std::lock_guard lock(m_sceneViewportMutex);
			settings = m_requestedSceneViewportSettings;
			enabled = m_requestedSceneViewportEnabled;
		}
		if (!enabled)
		{
			return swapchainExtent;
		}

		switch (settings.resolutionMode)
		{
			case SceneViewportResolutionMode::Fixed720p:
				return {1280, 720};
			case SceneViewportResolutionMode::Fixed1080p:
				return {1920, 1080};
			case SceneViewportResolutionMode::Fixed1440p:
				return {2560, 1440};
			case SceneViewportResolutionMode::Custom:
				return clampExtent(settings.customExtent);
			case SceneViewportResolutionMode::WindowNative:
			default:
				return clampExtent(swapchainExtent);
		}
	}

	SceneViewportSettings RenderingSubsystem::GetSceneViewportSettings() const
	{
		std::lock_guard lock(m_sceneViewportMutex);
		return m_requestedSceneViewportSettings;
	}

	bool RenderingSubsystem::IsSceneViewportEnabled() const
	{
		std::lock_guard lock(m_sceneViewportMutex);
		return m_requestedSceneViewportEnabled;
	}

	void RenderingSubsystem::DestroySceneViewportDepth()
	{
		if (m_bindlessManager != nullptr && m_sceneDepthBindlessSlot != 0xFFFFFFFFu)
		{
			m_bindlessManager->FreeSampledImageSlot(m_sceneDepthBindlessSlot);
		}
		m_sceneDepthBindlessSlot = 0xFFFFFFFFu;
		if (m_sceneDepthHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_sceneDepthHandle);
			m_sceneDepthHandle = {};
		}
		m_sceneDepth = {};
	}

	void RenderingSubsystem::CreateSceneViewportDepth(gpu::Device device, gpu::Format depthFormat, RenderGraph& graph, BindlessManager& bindless)
	{
		(void) device;
		const gpu::Extent2D extent = m_postProcessStack.GetExtent();
		m_sceneDepthHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = depthFormat,
		        .extent = extent,
		        .usage = gpu::ImageUsage::DepthStencilAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Depth,
		        .debugName = "Scene.Depth",
		});
		if (!m_sceneDepthHandle.IsValid())
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.Depth CreateTexture failed"));
		}

		const auto depthTexture = gpu::ResourceRegistry::ResolveTexture(m_sceneDepthHandle);
		m_sceneDepth = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_sceneDepthHandle), depthTexture.view, gpu::ImageAspect::Depth);

		const auto slot = bindless.AllocateSampledImageSlot();
		if (!slot)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.Depth AllocateSampledImageSlot failed"));
		}
		m_sceneDepthBindlessSlot = *slot;
		AE_EXPECT_OR_THROW_VOID(bindless.WriteSampledImage(m_sceneDepthBindlessSlot, gpu::ResourceRegistry::GetViewCreateInfo(m_sceneDepthHandle), gpu::ImageLayout::ShaderReadOnly));
	}

	void RenderingSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& vk = services.Get<VulkanContext>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();
		auto& cameras = services.Get<CameraManager>();
		auto& lighting = services.Get<LightingManager>();
		auto& materials = services.Get<MaterialBuffer>();
		auto& gpu = services.Get<GpuDevice>();
		m_bindlessManager = &bindless;

		m_renderGraph.Initialize(static_cast<void*>(vk.GetDevice().device), static_cast<void*>(vk.GetAllocator()));
		m_renderGraph.SetVulkanContext(&vk);
		m_renderGraph.SetDiagnosticEngine(&services.Get<DiagnosticEngine>());
		m_frameConstantsBuffer.Initialize();

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			const gpu::MappedBufferDesc desc{
			        .size = sizeof(ResourceEntry) * kFrameResourceCount,
			        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = "ResourceTable",
			};
			m_resourceTableBuffers[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!m_resourceTableBuffers[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderingSubsystem: ResourceTable CreateMappedBuffer failed"));
			}
			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_resourceTableBuffers[i].handle);
			m_resourceTableBuffers[i].mapped = view.mappedPtr;
			m_resourceTableBuffers[i].address = view.deviceAddress;
		}

		m_renderQueuePipelines.Initialize(vk.GetDevice().device);

		m_renderQueue.Initialize(m_renderQueuePipelines, RenderQueueConfig{.maxDraws = kRenderQueueMaxDraws, .debugName = "Main"});
		m_renderQueue.SetDebugForceVisible(true);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_renderQueue.SetDebugDisableAnimation(false);
		m_renderQueue.SetDebugAnimPassMask(0xFu);

		m_shadowService.Initialize(vk, swapchain, bindless, m_renderQueuePipelines);
		m_localShadowService.Initialize(vk, bindless, swapchain, m_renderQueuePipelines);
		m_renderTargetService.Initialize(vk, m_renderQueuePipelines);
		m_cullPass.Initialize(vk.GetDevice().device);

		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		CreateSceneViewportDepth(vk.GetDevice().device, swapchain.GetDepthFormat(), m_renderGraph, bindless);

		m_gtaoPass.Create({
		        .device = vk.GetDevice().device,
		        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});

		m_renderer.Initialize(&m_postProcessStack);

		AE_EXPECT_OR_THROW(skyboxPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://skybox.spv",
		                        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .debugName = "Skybox",
		                }));
		m_skyboxPipeline = std::move(skyboxPipeline);

		AE_EXPECT_OR_THROW(preDepthPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://shadow_depth.spv",
		                        .colorFormat = gpu::Format::Undefined,
		                        .depthFormat = swapchain.GetDepthFormat(),
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .debugName = "Scene.PreDepth",
		                }));
		m_preDepthPipeline = std::move(preDepthPipeline);

		m_renderTargetService.BindRuntime(FrameContext{
		        .graph = &m_renderGraph,
		        .bindless = &bindless,
		        .cameras = &cameras,
		        .lighting = &lighting,
		        .renderer = &m_renderer,
		        .materials = &materials,
		        .cullPass = &m_cullPass,
		        .frameIndex = [this]() { return m_frameIndexProvider ? m_frameIndexProvider() : 0ULL; },
		        .depthFormat = swapchain.GetDepthFormat(),
		        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		        .featureFlags = {.forwardEnabled = IsForwardPassEnabled()},
		});

		m_physicsDebug.Init(gpu, swapchain.GetImageFormat(), swapchain.GetDepthFormat());

		RegisterPasses(services);
	}

	void RenderingSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		DestroySceneViewportDepth();
		m_gtaoPass.Destroy();
		m_postProcessStack.Destroy();
		m_preDepthPipeline.Destroy();
		m_skyboxPipeline.Destroy();
		m_cullPass.Shutdown();
		m_frameConstantsBuffer.Shutdown();

		for (auto& buf: m_resourceTableBuffers)
		{
			if (buf.handle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf.handle);
			}
			buf.handle = {};
			buf.mapped = nullptr;
			buf.address = 0;
		}

		m_renderQueue.Shutdown();
		m_shadowService.Shutdown();
		m_localShadowService.Shutdown();
		m_renderTargetService.Shutdown();
		m_renderGraph.Shutdown();
		m_renderQueuePipelines.Shutdown();
		m_physicsDebug.Shutdown();
		m_bindlessManager = nullptr;
	}

	void RenderingSubsystem::RecreateSwapchainResources(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& gpu = services.Get<GpuDevice>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();

		// Scene-viewport rebuilds can be requested independently of a swapchain
		// recreate. Previous frames may still sample the old scene depth, GTAO,
		// or postprocess textures through bindless descriptors, so retire GPU
		// work before destroying and reusing those images/descriptors.
		gpu.WaitIdle();

		m_shadowService.RecreatePipeline(gpu.GetDevice(), swapchain.GetDepthFormat());
		m_preDepthPipeline.Destroy();
		AE_EXPECT_OR_THROW(preDepthPipeline,
		        GraphicsPipeline::Create(gpu.GetDevice(),
		                {
		                        .shaderVfsPath = "shaders://shadow_depth.spv",
		                        .colorFormat = gpu::Format::Undefined,
		                        .depthFormat = swapchain.GetDepthFormat(),
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .debugName = "Scene.PreDepth",
		                }));
		m_preDepthPipeline = std::move(preDepthPipeline);

		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float exposure = m_postProcessStack.GetExposure();
		const bool fxaaEnabled = m_postProcessStack.IsFxaaEnabled();

		m_renderQueue.DiscardAllPending();
		m_shadowService.ClearAllQueues();
		m_localShadowService.ClearAllQueues();
		m_renderTargetService.ClearAllQueues();
		DestroySceneViewportDepth();
		m_gtaoPass.Destroy();
		m_postProcessStack.Destroy();
		m_renderGraph.Clear();
		m_postProcessStack = PostProcessStack::Create({
		        .device = gpu.GetDevice(),
		        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		CreateSceneViewportDepth(gpu.GetDevice(), swapchain.GetDepthFormat(), m_renderGraph, bindless);
		m_gtaoPass.Create({
		        .device = gpu.GetDevice(),
		        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		m_postProcessStack.SetTonemapMode(tonemapMode);
		m_postProcessStack.SetExposure(exposure);
		m_postProcessStack.SetFxaaEnabled(fxaaEnabled);
		m_postProcessStack.SetOutputToTexture(m_sceneViewportEnabled);

		m_renderTargetService.OnRenderGraphReset(gpu.GetDevice(), swapchain.GetDepthFormat(), PostProcessStack::GetForwardColorFormat());

		services.Get<LightingManager>().RegisterPasses(m_renderGraph);
		RegisterPasses(services);
	}

	void RenderingSubsystem::SetSceneViewportEnabled(ServiceContainer& services, bool enabled)
	{
		{
			std::lock_guard lock(m_sceneViewportMutex);
			if (m_requestedSceneViewportEnabled == enabled)
			{
				return;
			}
			m_requestedSceneViewportEnabled = enabled;
		}
		m_sceneViewportRebuildPending.store(true, std::memory_order_release);
		(void) services;
	}

	void RenderingSubsystem::SetSceneViewportSettings(ServiceContainer& services, const SceneViewportSettings& settings)
	{
		SceneViewportSettings next = settings;
		next.customExtent.width = std::clamp(next.customExtent.width, 64u, 8192u);
		next.customExtent.height = std::clamp(next.customExtent.height, 64u, 8192u);
		{
			std::lock_guard lock(m_sceneViewportMutex);
			if (m_requestedSceneViewportSettings.resolutionMode == next.resolutionMode && m_requestedSceneViewportSettings.customExtent.width == next.customExtent.width
			        && m_requestedSceneViewportSettings.customExtent.height == next.customExtent.height)
			{
				return;
			}
			m_requestedSceneViewportSettings = next;
		}

		m_sceneViewportRebuildPending.store(true, std::memory_order_release);
		(void) services;
	}

	bool RenderingSubsystem::CommitPendingSceneViewportSettings()
	{
		if (!m_sceneViewportRebuildPending.exchange(false, std::memory_order_acq_rel))
		{
			return false;
		}

		std::lock_guard lock(m_sceneViewportMutex);
		m_sceneViewportEnabled = m_requestedSceneViewportEnabled;
		m_sceneViewportSettings = m_requestedSceneViewportSettings;
		return true;
	}

	void RenderingSubsystem::ApplyPendingSceneViewportChanges(ServiceContainer& services)
	{
		if (CommitPendingSceneViewportSettings())
		{
			RecreateSwapchainResources(services);
		}
	}

	void RenderingSubsystem::DiscardPendingFrameQueues(const std::uint32_t slot)
	{
		m_renderQueue.DiscardPending(slot);
		m_shadowService.DiscardPendingQueue(slot);
		m_localShadowService.DiscardPendingQueue(slot);
		m_renderTargetService.DiscardPendingQueues(slot);
	}

	void RenderingSubsystem::RegisterPasses(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& lightingManager = services.Get<LightingManager>();
		auto& bindless = services.Get<BindlessManager>();
		auto& swapchain = services.Get<Swapchain>();

		const FrameContext frame{
		        .graph = &m_renderGraph,
		        .bindless = &bindless,
		        .cameras = &services.Get<CameraManager>(),
		        .lighting = &lightingManager,
		        .renderer = &m_renderer,
		        .materials = &services.Get<MaterialBuffer>(),
		        .cullPass = &m_cullPass,
		        .frameIndex = [this]() { return m_frameIndexProvider ? m_frameIndexProvider() : 0ULL; },
		        .depthFormat = swapchain.GetDepthFormat(),
		        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		        .featureFlags = {.forwardEnabled = IsForwardPassEnabled()},
		};

		m_shadowService.SetupPassResources(m_renderGraph);
		m_localShadowService.SetupPassResources(m_renderGraph);

		m_shadowService.RegisterComputePasses(m_renderGraph, m_cullPass);
		m_localShadowService.RegisterComputePasses(m_renderGraph, m_cullPass);
		(void) m_renderGraph.CreatePreparedDrawList("MainSceneDraws");
		auto& blackboard = m_renderGraph.GetBlackboard();
		const PreparedDrawList mainSceneDraws = blackboard.Require<PreparedDrawList>("MainSceneDraws");
		const gpu::Extent2D sceneExtent = m_postProcessStack.GetExtent();
		const RGImage hdrColor = m_postProcessStack.GetHdrColor();
		(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductHdrColor},
		        FrameTextureProduct{
		                .image = hdrColor,
		                .extent = sceneExtent,
		                .format = PostProcessStack::GetForwardColorFormat(),
		                .bindlessSlot = m_postProcessStack.GetHdrBindlessSlot(),
		        },
		        FrameBlackboard::ProductMetadata{
		                .extent = sceneExtent,
		                .format = PostProcessStack::GetForwardColorFormat(),
		                .bindlessSlot = m_postProcessStack.GetHdrBindlessSlot(),
		        });
		if (m_sceneDepth.IsValid())
		{
			(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductSceneDepth},
			        FrameTextureProduct{
			                .image = m_sceneDepth,
			                .extent = sceneExtent,
			                .format = swapchain.GetDepthFormat(),
			                .bindlessSlot = m_sceneDepthBindlessSlot,
			        },
			        FrameBlackboard::ProductMetadata{
			                .extent = sceneExtent,
			                .format = swapchain.GetDepthFormat(),
			                .bindlessSlot = m_sceneDepthBindlessSlot,
			        });
		}
		if (m_gtaoPass.GetAoImage().IsValid())
		{
			(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductGtao},
			        FrameTextureProduct{
			                .image = m_gtaoPass.GetAoImage(),
			                .extent = m_gtaoPass.GetAoExtent(),
			                .format = gpu::Format::R8Unorm,
			                .bindlessSlot = m_gtaoPass.GetAoBindlessSlot(),
			        },
			        FrameBlackboard::ProductMetadata{
			                .extent = m_gtaoPass.GetAoExtent(),
			                .format = gpu::Format::R8Unorm,
			                .bindlessSlot = m_gtaoPass.GetAoBindlessSlot(),
			        });
		}
		m_cullPass.RegisterPass(m_renderGraph, m_renderQueue, {}, mainSceneDraws);

		if (m_sceneDepth.IsValid())
		{
			m_renderGraph
			        .AddDepthOnlyPass({
			                .name = "$ScenePreDepth",
			                .depth = m_sceneDepth,
			                .draws = mainSceneDraws,
			                .extent = sceneExtent,
			                .consumes = {RenderGraph::Product<MainViewProduct>(kFrameProductMainView)},
			                .produces = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth)},
			        })
			        .Execute(
			                [this](PassContext& ctx)
			                {
				                if (!IsForwardPassEnabled())
				                {
					                return;
				                }
				                m_renderQueue.FlushDrawWithFrameAddr(ctx.recorder, ctx.frameSlot, nullptr, ctx.frameConstantsAddr, &m_preDepthPipeline);
			                });
		}

		{
			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$Skybox",
			                .color = hdrColor,
			                .extent = sceneExtent,
			                .loadOp = gpu::LoadOp::Clear,
			                .consumes = {RenderGraph::Product<MainViewProduct>(kFrameProductMainView)},
			        })
			        .Execute(
			                [this](PassContext& ctx)
			                {
				                gpu::CommandList& cmd = ctx.recorder;
				                cmd.BindPipeline(m_skyboxPipeline.GetPipeline());
				                const gpu::DeviceAddress frameAddr = ctx.frameConstantsAddr;
				                std::byte bytes[sizeof(gpu::DeviceAddress)];
				                std::memcpy(bytes, &frameAddr, sizeof(bytes));
				                cmd.PushDataRaw(0, std::span<const std::byte>(bytes, sizeof(bytes)));
				                cmd.Draw(3);
			                });
		}
		m_shadowService.RegisterGraphicsPasses(m_renderGraph);
		m_localShadowService.RegisterGraphicsPasses(m_renderGraph);
		m_gtaoPass.RegisterPasses(m_renderGraph, m_sceneDepth);

		{
			const RGImage depth = m_sceneDepth.IsValid() ? m_sceneDepth : m_renderGraph.GetSwapchainDepth();
			std::vector<RenderGraph::FrameProductRef> forwardConsumes{
			        RenderGraph::Product<MainViewProduct>(kFrameProductMainView),
			};
			if (m_sceneDepth.IsValid())
			{
				forwardConsumes.push_back(RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth));
			}
			if (frame.lighting != nullptr)
			{
				forwardConsumes.push_back(RenderGraph::Product<LightBuffersProduct>(kFrameProductLightBuffers));
			}
			auto pass = m_renderGraph.AddDrawQueuePass({
			        .name = "$EngineForward",
			        .color = hdrColor,
			        .depth = depth,
			        .draws = mainSceneDraws,
			        .extent = sceneExtent,
			        .consumes = std::move(forwardConsumes),
			        .produces = {RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)},
			});

			pass.ConsumeTextureProduct<FrameTextureArrayProduct>(kFrameProductDirectionalShadows, FrameResourceId::DirectionalShadowC0);
			pass.ConsumeTextureProduct<LocalShadowProduct>(kFrameProductLocalShadows, FrameResourceId::LocalShadowAtlas);
			pass.ConsumeTextureProduct<FrameTextureProduct>(kFrameProductGtao, FrameResourceId::Gtao);

			if (auto* lighting = frame.lighting)
			{
				pass.ReadBuffer(lighting->GetLightsBufferHandle());
				pass.ReadBuffer(lighting->GetTileHeadersBufferHandle());
				pass.ReadBuffer(lighting->GetTileIndicesBufferHandle());
			}

			pass.Execute(
			        [this, &m_renderQueue = m_renderQueue, bindless = frame.bindless, lighting = frame.lighting](PassContext& ctx)
			        {
				        if (!IsForwardPassEnabled())
				        {
					        return;
				        }
				        const auto frameSlot = ctx.frameSlot;
				        const DrawContracts::LightingAddresses lightingAddr = lighting != nullptr ? lighting->GetLightingAddresses(frameSlot) : DrawContracts::LightingAddresses{};
				        bindless->CmdBindHeaps(ctx.recorder);
				        m_renderQueue.FlushDrawPush(ctx.recorder, ctx.frameSlot, lightingAddr);
			        });
		}

		m_renderTargetService.RegisterPasses();
		m_postProcessStack.SetOutputToTexture(m_sceneViewportEnabled);
		m_postProcessStack.RegisterPasses(m_renderGraph, *frame.bindless);
		m_physicsDebug.RegisterPass(m_renderGraph, m_sceneViewportEnabled ? m_postProcessStack.GetFinalColor() : RGImage{}, m_sceneViewportEnabled ? m_sceneDepth : RGImage{}, m_sceneViewportEnabled ? sceneExtent : gpu::Extent2D{});
		if (m_sceneViewportEnabled)
		{
			m_renderGraph.AddPass("$SceneViewportReady").ReadTexture(m_postProcessStack.GetFinalColor()).Execute([](PassContext&) {});

			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$SceneViewportClearSwapchain",
			                .color = m_renderGraph.GetSwapchainColor(),
			                .loadOp = gpu::LoadOp::Clear,
			        })
			        .Execute([](PassContext&) {});
		}
	}

	void RenderingSubsystem::WriteResourceTable(std::uint32_t frameIndex, std::span<const ResourceEntry> entries)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(entries.size() <= kFrameResourceCount, "ResourceEntry count exceeds kFrameResourceCount");
		const std::size_t byteCount = entries.size() * sizeof(ResourceEntry);
		std::memcpy(m_resourceTableBuffers[frameIndex].mapped, entries.data(), byteCount);
		gpu::ResourceRegistry::FlushMappedBuffer(m_resourceTableBuffers[frameIndex].handle, 0, static_cast<gpu::DeviceSize>(byteCount));
	}

	gpu::DeviceAddress RenderingSubsystem::PublishFrameResourceTable(std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		std::array<ResourceEntry, kFrameResourceCount> resourceTable{};
		resourceTable.fill(ResourceEntry{});

		m_renderGraph.PopulateResourceTable(std::span(resourceTable));

		if (!m_shadowService.IsDirectionalShadowEnabledForFrame(frameIndex))
		{
			for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
			{
				const auto id = static_cast<std::size_t>(static_cast<std::uint32_t>(FrameResourceId::DirectionalShadowC0) + cascade);
				resourceTable[id] = ResourceEntry{};
			}
		}

		WriteResourceTable(frameIndex, std::span(resourceTable));
		return GetResourceTableAddress(frameIndex);
	}
} // namespace aether
