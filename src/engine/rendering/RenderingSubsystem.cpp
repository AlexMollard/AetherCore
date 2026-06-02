#include "rendering/RenderingSubsystem.hpp"

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/LightingManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "material/MaterialBuffer.hpp"
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

		m_renderGraph.Initialize(vk.GetDevice().device, vk.GetAllocator());
		m_renderGraph.SetTracyVkCtx(vk.GetTracyVkCtx());
		m_frameConstantsBuffer.Initialize(vk);

		m_renderQueuePipelines.Initialize(vk.GetDevice().device);

		m_renderQueue.Initialize(vk.GetDevice().device, vk.GetAllocator(), m_renderQueuePipelines, RenderQueue::Config{ .maxDraws = 65536 });
		m_renderQueue.SetDebugForceVisible(false);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_renderQueue.SetTracyVkCtx(vk.GetTracyVkCtx());

		m_shadowService.Initialize(vk, swapchain, m_renderQueuePipelines);
		m_localShadowService.Initialize(vk, bindless, swapchain, m_renderQueuePipelines);
		m_renderTargetService.Initialize(vk, m_renderQueuePipelines);
		m_cullPass.Initialize(vk.GetDevice().device);

		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .allocator = vk.GetAllocator(),
		        .extent = swapchain.GetExtent(),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});

		m_renderer.Initialize(&m_postProcessStack);

		m_skyboxPass = SkyboxPass::Create({
		        .device = vk.GetDevice().device,
		        .hdrColorFormat = PostProcessStack::GetForwardColorFormat(),
		});

		m_renderTargetService.BindRuntime(
		        m_renderGraph,
		        bindless,
		        cameras,
		        lighting,
		        m_renderer,
		        materials,
		        m_cullPass,
		        /*frameIndexCallback=*/[this]() { return m_frameIndexProvider ? m_frameIndexProvider() : 0ULL; },
		        vk.GetDevice().device,
		        swapchain.GetDepthFormat(),
		        PostProcessStack::GetForwardColorFormat());

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
	}

	void RenderingSubsystem::RecreateSwapchainResources(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		VulkanContext& vk = services.Get<VulkanContext>();
		Swapchain& swapchain = services.Get<Swapchain>();
		BindlessManager& bindless = services.Get<BindlessManager>();

		m_shadowService.RecreatePipeline(vk.GetDevice().device, swapchain.GetDepthFormat());

		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float exposure = m_postProcessStack.GetExposure();
		const bool fxaaEnabled = m_postProcessStack.IsFxaaEnabled();

		m_postProcessStack.Destroy();
		m_renderGraph.Clear();
		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .allocator = vk.GetAllocator(),
		        .extent = swapchain.GetExtent(),
		        .swapchainFormat = swapchain.GetImageFormat(),
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

		const PassRegistrationContext ctx{
			.graph = m_renderGraph,
			.skyboxPass = m_skyboxPass,
			.postProcessStack = m_postProcessStack,
			.shadowService = m_shadowService,
			.localShadowService = m_localShadowService,
			.bindlessManager = services.Get<BindlessManager>(),
			.device = services.Get<VulkanContext>().GetDevice().device,
			.depthFormat = services.Get<Swapchain>().GetDepthFormat(),
			.cullPass = m_cullPass,
			.mainRenderQueue = m_renderQueue,
			.forwardPass = m_forwardPass,
			.getLightingSet = [this, &lighting]()
			{
				const auto frameIdx = static_cast<std::uint32_t>((m_frameIndexProvider ? m_frameIndexProvider() : 0ULL) % Swapchain::kMaxFramesInFlight);
				return lighting.GetSet(frameIdx);
			},
			.renderTargetService = m_renderTargetService,
		};

		m_renderPipelineCoordinator.RegisterPasses(ctx);
	}
} // namespace aether
