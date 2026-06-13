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

		// -- Phase 1: Setup pass resources (creates RGImage handles) ----------
		ctx.shadowService.SetupPassResources(*frame.graph, *frame.bindless, ctx.device, frame.depthFormat);
		ctx.localShadowService.SetupPassResources(*frame.graph, frame.depthFormat);

		// -- Phase 2: Async compute passes (all before any graphics pass) -----
		ctx.shadowService.RegisterComputePasses(*frame.graph, ctx.cullPass);
		ctx.localShadowService.RegisterComputePasses(*frame.graph, ctx.cullPass);
		ctx.cullPass.RegisterPass(*frame.graph, ctx.mainRenderQueue);

		// -- Phase 3: Graphics passes ----------------------------------------
		// Pass 1: sky background.
		ctx.skyboxPass.RegisterPass(*frame.graph, ctx.postProcessStack.GetHdrColor());

		// Passes 2..4: directional shadow cascades.
		ctx.shadowService.RegisterGraphicsPasses(*frame.graph);

		// Local shadow atlas render + VSM blur (graphics-queue compute).
		ctx.localShadowService.RegisterGraphicsPasses(*frame.graph);

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
