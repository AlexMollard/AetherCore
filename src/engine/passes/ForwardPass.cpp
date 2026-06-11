#include "passes/ForwardPass.hpp"

#include "gpu/BindlessManager.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/RenderQueue.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void ForwardPass::RegisterPass(
	        const FrameContext& frame, RenderQueue& renderQueue, RGImage hdrColor, RGImage depth, std::function<void(gpu::CommandList&, gpu::PipelineLayout)> pushLightingFn, std::span<const RGImage> shadowMaps, RGImage localShadowAtlas)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(frame.graph != nullptr, "ForwardPass::RegisterPass: frame.graph is null");
		AE_ASSERT(frame.bindless != nullptr, "ForwardPass::RegisterPass: frame.bindless is null");

		RenderGraph& graph = *frame.graph;
		const gpu::DescriptorSet bindlessSet = frame.bindless->GetSet();

		auto* pass = &graph.AddPass("$EngineForward").WriteColor(hdrColor, gpu::LoadOp::Load, gpu::StoreOp::Store).WriteDepth(depth, gpu::LoadOp::Clear, gpu::StoreOp::DontCare, ClearDepthValue(1.0f));

		for (const RGImage shadowMap: shadowMaps)
		{
			if (shadowMap.IsValid())
			{
				pass->ReadTexture(shadowMap);
			}
		}

		if (localShadowAtlas.IsValid())
		{
			pass->ReadTexture(localShadowAtlas);
		}

		pass->Execute(
		        [&renderQueue, bindlessSet, pushLightingFn, forwardEnabled = frame.featureFlags.forwardEnabled](PassContext& ctx)
		        {
			        AE_PROFILE_ZONE();
			        if (!forwardEnabled)
			        {
				        return;
			        }
			        renderQueue.FlushDrawPush(ctx.recorder, bindlessSet, pushLightingFn);
			        renderQueue.Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
		        });
	}
} // namespace aether
