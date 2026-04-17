#include "RenderPipelineCoordinator.hpp"

#include "BindlessManager.hpp"
#include "CullPass.hpp"
#include "ForwardPass.hpp"
#include "PostProcessStack.hpp"
#include "RenderQueue.hpp"
#include "RenderTargetService.hpp"
#include "ShadowService.hpp"
#include "SkyboxPass.hpp"

namespace aether
{
	void RenderPipelineCoordinator::RegisterPasses(RenderGraph& graph,
	        SkyboxPass& skyboxPass,
	        PostProcessStack& postProcessStack,
	        ShadowService& shadowService,
	        BindlessManager& bindlessManager,
	        const VkDevice device,
	        const VkFormat depthFormat,
	        CullPass& cullPass,
	        RenderQueue& mainRenderQueue,
	        ForwardPass& forwardPass,
	        std::function<VkDescriptorSet()> getLightingSet,
	        RenderTargetService& renderTargetService)
	{
		// Pass 1: sky background.
		skyboxPass.RegisterPass(graph, postProcessStack.GetHdrColor());

		// Passes 2..N: shadow cascades.
		shadowService.RegisterPasses(graph, bindlessManager, device, cullPass, depthFormat);

		// Main camera cull pass.
		cullPass.RegisterPass(graph, mainRenderQueue);

		// Main camera forward lighting pass.
		forwardPass.RegisterPass(graph, postProcessStack.GetHdrColor(), graph.GetSwapchainDepth(), mainRenderQueue, bindlessManager.GetSet(), std::move(getLightingSet), shadowService.GetShadowDepthImages());

		// RTT camera pass set.
		renderTargetService.RegisterPasses();

		// Post-processing chain.
		postProcessStack.RegisterPasses(graph, bindlessManager);
	}
} // namespace aether
