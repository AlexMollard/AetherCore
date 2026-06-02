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
	void RenderPipelineCoordinator::RegisterPasses(const PassRegistrationContext& ctx)
	{
		// Pass 1: sky background.
		ctx.skyboxPass.RegisterPass(ctx.graph, ctx.postProcessStack.GetHdrColor());

		// Passes 2..N: shadow cascades (directional CSM).
		ctx.shadowService.RegisterPasses(ctx.graph, ctx.bindlessManager, ctx.device, ctx.cullPass, ctx.depthFormat);

		// Local shadow passes: atlas setup, cull, render, blur.
		ctx.localShadowService.RegisterPasses(ctx.graph, ctx.bindlessManager, ctx.device, ctx.cullPass, ctx.depthFormat);

		// Main camera cull pass.
		ctx.cullPass.RegisterPass(ctx.graph, ctx.mainRenderQueue);

		// Main camera forward lighting pass.
		ctx.forwardPass.RegisterPass(ctx.graph, ctx.postProcessStack.GetHdrColor(), ctx.graph.GetSwapchainDepth(), ctx.mainRenderQueue, ctx.bindlessManager.GetSet(), std::move(ctx.getLightingSet), ctx.shadowService.GetShadowDepthImages(), ctx.localShadowService.GetAtlasRGImage());

		// RTT camera pass set.
		ctx.renderTargetService.RegisterPasses();

		// Post-processing chain.
		ctx.postProcessStack.RegisterPasses(ctx.graph, ctx.bindlessManager);
	}
} // namespace aether
