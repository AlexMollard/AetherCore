#pragma once

#include <cstdint>
#include <functional>

#include "passes/CullPass.hpp"
#include "passes/ForwardPass.hpp"
#include "rendering/FrameComposer.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderPipelineCoordinator.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "passes/SkyboxPass.hpp"

class ServiceContainer;

namespace aether
{
	// Owns all rendering passes and the render graph. Depends on Vulkan context,
	// camera, lighting, and material services from the container.
	class RenderingSubsystem
	{
	public:
		void Init(ServiceContainer& services);
		void Shutdown(ServiceContainer& services);

		// Called on swapchain recreation to rebuild extent-dependent resources.
		void RecreateSwapchainResources(ServiceContainer& services);

		// Set by AetherCore after construction to provide the current frame index
		// for RTT queue preparation and pass callbacks.
		void SetFrameIndexProvider(std::function<std::uint64_t()> provider)
		{
			m_frameIndexProvider = std::move(provider);
		}

		[[nodiscard]] RenderGraph& GetRenderGraph()
		{
			return m_renderGraph;
		}

		[[nodiscard]] RenderQueue& GetRenderQueue()
		{
			return m_renderQueue;
		}

		[[nodiscard]] Renderer& GetRenderer()
		{
			return m_renderer;
		}

		[[nodiscard]] FrameConstantsBuffer& GetFrameConstantsBuffer()
		{
			return m_frameConstantsBuffer;
		}

		[[nodiscard]] ShadowService& GetShadowService()
		{
			return m_shadowService;
		}

		[[nodiscard]] RenderTargetService& GetRenderTargetService()
		{
			return m_renderTargetService;
		}

		[[nodiscard]] CullPass& GetCullPass()
		{
			return m_cullPass;
		}

		[[nodiscard]] ForwardPass& GetForwardPass()
		{
			return m_forwardPass;
		}

		[[nodiscard]] SkyboxPass& GetSkyboxPass()
		{
			return m_skyboxPass;
		}

		[[nodiscard]] PostProcessStack& GetPostProcessStack()
		{
			return m_postProcessStack;
		}

		[[nodiscard]] FrameComposer& GetFrameComposer()
		{
			return m_frameComposer;
		}

	private:
		void RegisterPasses(ServiceContainer& services);

		RenderQueueSharedPipelines m_renderQueuePipelines;
		RenderGraph m_renderGraph;
		RenderQueue m_renderQueue;
		Renderer m_renderer;
		FrameConstantsBuffer m_frameConstantsBuffer;
		ShadowService m_shadowService;
		RenderTargetService m_renderTargetService;
		CullPass m_cullPass;
		ForwardPass m_forwardPass;
		SkyboxPass m_skyboxPass;
		PostProcessStack m_postProcessStack;
		FrameComposer m_frameComposer;
		RenderPipelineCoordinator m_renderPipelineCoordinator;
		std::function<std::uint64_t()> m_frameIndexProvider;
	};
} // namespace aether
