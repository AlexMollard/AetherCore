#include "rendering/RenderPipelineCoordinator.hpp"

#include "passes/CullPass.hpp"
#include "passes/ForwardPass.hpp"
#include "passes/PostProcessStack.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "passes/SkyboxPass.hpp"
#include "vulkan/GpuEnumConversions.hpp"

namespace aether
{
	void RenderPipelineCoordinator::RegisterPasses(const PassRegistrationContext& ctx)
	{
		const FrameContext& frame = ctx.frame;

		// Pass 1: sky background.
		ctx.skyboxPass.RegisterPass(*frame.graph, ctx.postProcessStack.GetHdrColor());

		// Passes 2..N: shadow cascades (directional CSM).
		ctx.shadowService.RegisterPasses(*frame.graph, *frame.bindless, ctx.device, ctx.cullPass, frame.depthFormat);

		// Local shadow passes: atlas setup, cull, render, blur.
		ctx.localShadowService.RegisterPasses(*frame.graph, *frame.bindless, ctx.device, ctx.cullPass, frame.depthFormat);

		// Main camera cull pass.
		ctx.cullPass.RegisterPass(*frame.graph, ctx.mainRenderQueue);

		// Main camera forward lighting pass.
		ctx.forwardPass.RegisterPass(frame, ctx.mainRenderQueue, ctx.postProcessStack.GetHdrColor(), frame.graph->GetSwapchainDepth(), std::move(ctx.pushLightingFn), ctx.shadowService.GetShadowDepthImages(), ctx.localShadowService.GetAtlasRGImage());

		// RTT camera pass set.
		ctx.renderTargetService.RegisterPasses();

		// Post-processing chain.
		ctx.postProcessStack.RegisterPasses(*frame.graph, *frame.bindless);

		// Physics debug wireframe overlay (after post-processing, before final present).
		ctx.physicsDebug.RegisterPass(*frame.graph);
	}
} // namespace aether
