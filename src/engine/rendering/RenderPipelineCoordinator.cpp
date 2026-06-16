#include "rendering/RenderPipelineCoordinator.hpp"

#include "passes/CullPass.hpp"
#include "passes/ForwardPass.hpp"
#include "passes/PostProcessStack.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/LightingManager.hpp"
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

		ctx.shadowService.SetupPassResources(*frame.graph, ctx.device, frame.depthFormat);
		ctx.localShadowService.SetupPassResources(*frame.graph, frame.depthFormat);

		ctx.shadowService.RegisterComputePasses(*frame.graph, ctx.cullPass);
		ctx.localShadowService.RegisterComputePasses(*frame.graph, ctx.cullPass);
		ctx.cullPass.RegisterPass(*frame.graph, ctx.mainRenderQueue);

		ctx.skyboxPass.RegisterPass(*frame.graph, ctx.postProcessStack.GetHdrColor());
		ctx.shadowService.RegisterGraphicsPasses(*frame.graph);
		ctx.localShadowService.RegisterGraphicsPasses(*frame.graph);

		ctx.forwardPass.RegisterPass(frame,
		        ctx.mainRenderQueue,
		        ctx.postProcessStack.GetHdrColor(),
		        frame.graph->GetSwapchainDepth(),
		        std::move(ctx.pushLightingFn),
		        ctx.shadowService.GetShadowDepthImages(),
		        ctx.localShadowService.GetAtlasRGImage(),
		        frame.lighting ? frame.lighting->GetLightsBufferHandle() : RGBuffer{},
		        frame.lighting ? frame.lighting->GetTileHeadersBufferHandle() : RGBuffer{},
		        frame.lighting ? frame.lighting->GetTileIndicesBufferHandle() : RGBuffer{});

		ctx.renderTargetService.RegisterPasses();
		ctx.postProcessStack.RegisterPasses(*frame.graph, *frame.bindless);
		ctx.physicsDebug.RegisterPass(*frame.graph);
	}
} // namespace aether
