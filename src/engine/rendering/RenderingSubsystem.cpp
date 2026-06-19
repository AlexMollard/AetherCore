#include "rendering/RenderingSubsystem.hpp"

#include <cstring>
#include <span>

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/LightingManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/MaterialBuffer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/DiagnosticEngine.hpp"

namespace aether
{
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

		m_renderGraph.Initialize(static_cast<void*>(vk.GetDevice().device), static_cast<void*>(vk.GetAllocator()));
		m_renderGraph.SetVulkanContext(&vk);
		m_renderGraph.SetDiagnosticEngine(&services.Get<DiagnosticEngine>());
		m_frameConstantsBuffer.Initialize(vk);

		m_renderQueuePipelines.Initialize(vk.GetDevice().device, vk.GetPipelineCache());

		m_renderQueue.Initialize(vk.GetDevice().device, vk.GetAllocator(), m_renderQueuePipelines, RenderQueueConfig{.maxDraws = 65536});
		m_renderQueue.SetDebugForceVisible(true);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_renderQueue.SetDebugDisableAnimation(false);
		m_renderQueue.SetDebugAnimPassMask(0xFu);

		m_shadowService.Initialize(vk, swapchain, m_renderQueuePipelines);
		m_localShadowService.Initialize(vk, bindless, swapchain, m_renderQueuePipelines);
		m_renderTargetService.Initialize(vk, m_renderQueuePipelines);
		m_cullPass.Initialize(vk.GetDevice().device, vk.GetPipelineCache());

		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .pipelineCache = vk.GetPipelineCache(),
		        .allocator = vk.GetAllocator(),
		        .extent = swapchain.GetExtent(),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});

		m_renderer.Initialize(&m_postProcessStack);

		AE_EXPECT_OR_THROW(skyboxPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                vk.GetPipelineCache(),
		                {
		                        .shaderVfsPath = "shaders://skybox.spv",
		                        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .debugName = "Skybox",
		                }));
		m_skyboxPipeline = std::move(skyboxPipeline);

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
		        .featureFlags = {.forwardEnabled = m_forwardPassEnabled},
		});

		m_physicsDebug.Init(gpu, swapchain.GetImageFormat(), swapchain.GetDepthFormat());

		RegisterPasses(services);
	}

	void RenderingSubsystem::Shutdown(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& vk = services.Get<VulkanContext>();
		auto& gpu = services.Get<GpuDevice>();

		m_postProcessStack.Destroy();
		m_skyboxPipeline.Destroy();
		m_cullPass.Shutdown();
		m_frameConstantsBuffer.Shutdown();
		m_renderQueue.Shutdown();
		m_shadowService.Shutdown(gpu.GetDevice());
		m_localShadowService.Shutdown(gpu.GetDevice());
		m_renderTargetService.Shutdown();
		m_renderGraph.Shutdown();
		m_renderQueuePipelines.Shutdown(vk.GetDevice().device);
		m_physicsDebug.Shutdown();
	}

	void RenderingSubsystem::RecreateSwapchainResources(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& gpu = services.Get<GpuDevice>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();

		m_shadowService.RecreatePipeline(gpu.GetDevice(), gpu.GetPipelineCache(), swapchain.GetDepthFormat());

		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float exposure = m_postProcessStack.GetExposure();
		const bool fxaaEnabled = m_postProcessStack.IsFxaaEnabled();

		m_postProcessStack.Destroy();
		m_renderGraph.Clear();
		m_postProcessStack = PostProcessStack::Create({
		        .device = gpu.GetDevice(),
		        .pipelineCache = gpu.GetPipelineCache(),
		        .allocator = gpu.GetAllocator(),
		        .extent = swapchain.GetExtent(),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		m_postProcessStack.SetTonemapMode(tonemapMode);
		m_postProcessStack.SetExposure(exposure);
		m_postProcessStack.SetFxaaEnabled(fxaaEnabled);

		m_renderTargetService.OnRenderGraphReset(gpu.GetDevice(), swapchain.GetDepthFormat(), PostProcessStack::GetForwardColorFormat());

		RegisterPasses(services);
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
		        .featureFlags = {.forwardEnabled = m_forwardPassEnabled},
		};

		auto& gpu = services.Get<GpuDevice>();

		const auto frameIdx = static_cast<std::uint32_t>((m_frameIndexProvider ? m_frameIndexProvider() : 0ULL) % Swapchain::kMaxFramesInFlight);
		auto lightingAddr = frame.lighting ? frame.lighting->GetLightingAddresses(frameIdx) : DrawContracts::LightingAddresses{};

		m_shadowService.SetupPassResources(m_renderGraph, gpu.GetDevice(), frame.depthFormat);
		m_localShadowService.SetupPassResources(m_renderGraph, frame.depthFormat);

		m_shadowService.RegisterComputePasses(m_renderGraph, m_cullPass);
		m_localShadowService.RegisterComputePasses(m_renderGraph, m_cullPass);
		m_cullPass.RegisterPass(m_renderGraph, m_renderQueue);

		{
			const RGImage hdrColor = m_postProcessStack.GetHdrColor();
			m_renderGraph.AddPass("$Skybox")
			        .WriteColor(hdrColor, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f))
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

		{
			const RGImage hdrColor = m_postProcessStack.GetHdrColor();
			const RGImage depth = m_renderGraph.GetSwapchainDepth();
			auto* pass = &m_renderGraph.AddPass("$EngineForward").WriteColor(hdrColor, gpu::LoadOp::Load, gpu::StoreOp::Store).WriteDepth(depth, gpu::LoadOp::Clear, gpu::StoreOp::DontCare, ClearDepthValue(1.0f));

			for (const RGImage shadowMap: m_shadowService.GetShadowDepthImages())
			{
				if (shadowMap.IsValid())
				{
					pass->ReadTexture(shadowMap);
				}
			}

			const RGImage localShadowAtlas = m_localShadowService.GetAtlasRGImage();
			if (localShadowAtlas.IsValid())
			{
				pass->ReadTexture(localShadowAtlas);
			}

			if (auto* lighting = frame.lighting)
			{
				pass->ReadBuffer(lighting->GetLightsBufferHandle());
				pass->ReadBuffer(lighting->GetTileHeadersBufferHandle());
				pass->ReadBuffer(lighting->GetTileIndicesBufferHandle());
			}

			pass->Execute(
			        [&m_renderQueue = m_renderQueue, bindless = frame.bindless, lightingAddr, forwardEnabled = frame.featureFlags.forwardEnabled](PassContext& ctx)
			        {
				        if (!forwardEnabled)
				        {
					        return;
				        }
				        bindless->CmdBindHeaps(ctx.recorder);
				        m_renderQueue.FlushDrawPush(ctx.recorder, nullptr, lightingAddr);
				        m_renderQueue.Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
			        });
		}

		m_renderTargetService.RegisterPasses();
		m_postProcessStack.RegisterPasses(m_renderGraph, *frame.bindless);
		m_physicsDebug.RegisterPass(m_renderGraph);
	}
} // namespace aether
