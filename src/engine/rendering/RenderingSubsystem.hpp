#pragma once

#include <cstdint>
#include <functional>

#include "passes/CullPass.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "physics/PhysicsDebugRenderer.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	// Owns all rendering passes and the render graph. Depends on Vulkan context,
	// camera, lighting, and material services from the container.
	class RenderingSubsystem
	{
	public:
		void Init(ServiceContainer& services);
		void Shutdown();

		// Called on swapchain recreation to rebuild extent-dependent resources.
		void RecreateSwapchainResources(ServiceContainer& services);

		// Set by AetherCore after construction to provide the current frame index
		// for RTT queue preparation and pass callbacks.
		void SetFrameIndexProvider(std::function<std::uint64_t()> provider)
		{
			m_frameIndexProvider = std::move(provider);
		}

		void SetForwardPassEnabled(bool enabled)
		{
			m_forwardPassEnabled = enabled;
		}

		[[nodiscard]] bool IsForwardPassEnabled() const
		{
			return m_forwardPassEnabled;
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

		[[nodiscard]] LocalShadowService& GetLocalShadowService()
		{
			return m_localShadowService;
		}

		[[nodiscard]] RenderTargetService& GetRenderTargetService()
		{
			return m_renderTargetService;
		}

		[[nodiscard]] CullPass& GetCullPass()
		{
			return m_cullPass;
		}

		[[nodiscard]] PostProcessStack& GetPostProcessStack()
		{
			return m_postProcessStack;
		}

		[[nodiscard]] PhysicsDebugRenderer& GetPhysicsDebugRenderer()
		{
			return m_physicsDebug;
		}

	private:
		void RegisterPasses(ServiceContainer& services);

		RenderQueueSharedPipelines m_renderQueuePipelines;
		RenderGraph m_renderGraph;
		RenderQueue m_renderQueue;
		Renderer m_renderer;
		FrameConstantsBuffer m_frameConstantsBuffer;
		ShadowService m_shadowService;
		LocalShadowService m_localShadowService;
		RenderTargetService m_renderTargetService;
		CullPass m_cullPass;
		GraphicsPipeline m_skyboxPipeline;
		PostProcessStack m_postProcessStack;
		std::function<std::uint64_t()> m_frameIndexProvider;
		PhysicsDebugRenderer m_physicsDebug;
		bool m_forwardPassEnabled = true;
	};
} // namespace aether
