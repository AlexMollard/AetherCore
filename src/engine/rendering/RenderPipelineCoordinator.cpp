#include "rendering/RenderPipelineCoordinator.hpp"

#include "gpu/BindlessManager.hpp"
#include "passes/CullPass.hpp"
#include "passes/ForwardPass.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "passes/SkyboxPass.hpp"

namespace aether
{
	void RenderPipelineCoordinator::RegisterPasses(RenderGraph& graph,
	        SkyboxPass& skyboxPass,
	        PostProcessStack& postProcessStack,
	        ShadowService& shadowService,
	        LocalShadowService& localShadowService,
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

		// Passes 2..N: shadow cascades (directional CSM).
		shadowService.RegisterPasses(graph, bindlessManager, device, cullPass, depthFormat);

		// Local shadow passes: atlas setup, cull, render, blur.
		localShadowService.RegisterPasses(graph, bindlessManager, device, cullPass, depthFormat);

		// Main camera cull pass.
		cullPass.RegisterPass(graph, mainRenderQueue);

		// Main camera forward lighting pass.
		forwardPass.RegisterPass(graph, postProcessStack.GetHdrColor(), graph.GetSwapchainDepth(), mainRenderQueue, bindlessManager.GetSet(), std::move(getLightingSet), shadowService.GetShadowDepthImages(), localShadowService.GetAtlasRGImage());

		// RTT camera pass set.
		renderTargetService.RegisterPasses();

		// Post-processing chain.
		postProcessStack.RegisterPasses(graph, bindlessManager);
	}
} // namespace aether
