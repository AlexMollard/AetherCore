#include "rendering/RenderingSubsystem.hpp"

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/LightingManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/MaterialBuffer.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void RenderingSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		VulkanContext& vk = services.Get<VulkanContext>();
		Swapchain& swapchain = services.Get<Swapchain>();
		BindlessManager& bindless = services.Get<BindlessManager>();
		CameraManager& cameras = services.Get<CameraManager>();
		LightingManager& lighting = services.Get<LightingManager>();
		MaterialBuffer& materials = services.Get<MaterialBuffer>();
		GpuDevice& gpu = services.Get<GpuDevice>();

		m_renderGraph.Initialize(vk.GetDevice().device, vk.GetAllocator());
		m_renderGraph.SetTracyVkCtx(vk.GetTracyVkCtx());
		m_frameConstantsBuffer.Initialize(vk);

		m_renderQueuePipelines.Initialize(vk.GetDevice().device, vk.GetPipelineCache());

		m_renderQueue.Initialize(vk.GetDevice().device, vk.GetAllocator(), m_renderQueuePipelines, RenderQueueConfig{.maxDraws = 65536});
		m_renderQueue.SetDebugForceVisible(false);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_renderQueue.SetDebugDisableAnimation(false);
		m_renderQueue.SetDebugAnimPassMask(0xFu);
		m_renderQueue.SetTracyVkCtx(vk.GetTracyVkCtx());

		m_shadowService.Initialize(vk, swapchain, m_renderQueuePipelines);
		m_localShadowService.Initialize(vk, bindless, swapchain, m_renderQueuePipelines);
		m_renderTargetService.Initialize(vk, m_renderQueuePipelines);
		m_cullPass.Initialize(vk.GetDevice().device, vk.GetPipelineCache());

		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .pipelineCache = vk.GetPipelineCache(),
		        .allocator = vk.GetAllocator(),
		        .extent = swapchain.GetExtent(),
		        .swapchainFormat = gpu::ToVk(swapchain.GetImageFormat()),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});

		m_renderer.Initialize(&m_postProcessStack);

		m_skyboxPass = SkyboxPass::Create({
		        .device = vk.GetDevice().device,
		        .pipelineCache = vk.GetPipelineCache(),
		        .hdrColorFormat = PostProcessStack::GetForwardColorFormat(),
		});

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
		VulkanContext& vk = services.Get<VulkanContext>();

		m_postProcessStack.Destroy();
		m_skyboxPass.Destroy();
		m_cullPass.Shutdown();
		m_frameConstantsBuffer.Shutdown();
		m_renderQueue.Shutdown();
		m_shadowService.Shutdown(vk.GetDevice().device);
		m_localShadowService.Shutdown(vk.GetDevice().device);
		m_renderTargetService.Shutdown();
		m_renderGraph.Shutdown();
		m_renderQueuePipelines.Shutdown(vk.GetDevice().device);
		m_physicsDebug.Shutdown();
	}

	void RenderingSubsystem::RecreateSwapchainResources(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		VulkanContext& vk = services.Get<VulkanContext>();
		Swapchain& swapchain = services.Get<Swapchain>();
		BindlessManager& bindless = services.Get<BindlessManager>();

		m_shadowService.RecreatePipeline(vk.GetDevice().device, vk.GetPipelineCache(), swapchain.GetDepthFormat());

		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float exposure = m_postProcessStack.GetExposure();
		const bool fxaaEnabled = m_postProcessStack.IsFxaaEnabled();

		m_postProcessStack.Destroy();
		m_renderGraph.Clear();
		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .pipelineCache = vk.GetPipelineCache(),
		        .allocator = vk.GetAllocator(),
		        .extent = swapchain.GetExtent(),
		        .swapchainFormat = gpu::ToVk(swapchain.GetImageFormat()),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		m_postProcessStack.SetTonemapMode(tonemapMode);
		m_postProcessStack.SetExposure(exposure);
		m_postProcessStack.SetFxaaEnabled(fxaaEnabled);

		m_renderTargetService.OnRenderGraphReset(vk.GetDevice().device, swapchain.GetDepthFormat(), PostProcessStack::GetForwardColorFormat());

		RegisterPasses(services);
	}

	void RenderingSubsystem::RegisterPasses(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		LightingManager& lighting = services.Get<LightingManager>();
		BindlessManager& bindless = services.Get<BindlessManager>();
		Swapchain& swapchain = services.Get<Swapchain>();

		const FrameContext frame{
		        .graph = &m_renderGraph,
		        .bindless = &bindless,
		        .cameras = &services.Get<CameraManager>(),
		        .lighting = &lighting,
		        .renderer = &m_renderer,
		        .materials = &services.Get<MaterialBuffer>(),
		        .cullPass = &m_cullPass,
		        .frameIndex = [this]() { return m_frameIndexProvider ? m_frameIndexProvider() : 0ULL; },
		        .depthFormat = swapchain.GetDepthFormat(),
		        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		        .featureFlags = {.forwardEnabled = m_forwardPassEnabled},
		};

		const PassRegistrationContext ctx{
		        .frame = frame,
		        .device = services.Get<VulkanContext>().GetDevice().device,
		        .skyboxPass = m_skyboxPass,
		        .postProcessStack = m_postProcessStack,
		        .shadowService = m_shadowService,
		        .localShadowService = m_localShadowService,
		        .cullPass = m_cullPass,
		        .mainRenderQueue = m_renderQueue,
		        .forwardPass = m_forwardPass,
		        .pushLightingFn =
		                [this, &lighting](gpu::CommandList& cmd, gpu::PipelineLayout layout)
		        {
			        const auto frameIdx = static_cast<std::uint32_t>((m_frameIndexProvider ? m_frameIndexProvider() : 0ULL) % Swapchain::kMaxFramesInFlight);
			        lighting.PushLightingDescriptor(cmd, layout, frameIdx);
		        },
		        .renderTargetService = m_renderTargetService,
		        .physicsDebug = m_physicsDebug,
		};

		m_renderPipelineCoordinator.RegisterPasses(ctx);
	}
} // namespace aether
