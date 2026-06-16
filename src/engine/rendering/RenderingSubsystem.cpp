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
		auto& vk = services.Get<VulkanContext>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();
		auto& cameras = services.Get<CameraManager>();
		auto& lighting = services.Get<LightingManager>();
		auto& materials = services.Get<MaterialBuffer>();
		auto& gpu = services.Get<GpuDevice>();

		m_renderGraph.Initialize(static_cast<void*>(vk.GetDevice().device), static_cast<void*>(vk.GetAllocator()));
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
		auto& vk = services.Get<VulkanContext>();
		auto& gpu = services.Get<GpuDevice>();

		m_postProcessStack.Destroy();
		m_skyboxPass.Destroy();
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
		auto& lighting = services.Get<LightingManager>();
		auto& bindless = services.Get<BindlessManager>();
		auto& swapchain = services.Get<Swapchain>();

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
