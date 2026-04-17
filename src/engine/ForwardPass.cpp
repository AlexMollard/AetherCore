#include "ForwardPass.hpp"

#include "RenderQueue.hpp"

namespace aether
{
	void ForwardPass::RegisterPass(RenderGraph& graph, RGImage hdrColor, RGImage depth, RenderQueue& renderQueue, VkDescriptorSet bindlessSet, std::function<VkDescriptorSet()> getLightingSet, std::span<const RGImage> shadowMaps)
	{
		auto* pass = &graph.AddPass("$EngineForward").WriteColor(hdrColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE).WriteDepth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, ClearDepthValue(1.0f));

		for (const RGImage shadowMap: shadowMaps)
		{
			if (shadowMap.IsValid())
			{
				pass->ReadTexture(shadowMap);
			}
		}

		pass->Execute(
		        [&renderQueue, bindlessSet, getLightingSet](PassContext& ctx)
		        {
			        renderQueue.FlushDraw(ctx.recorder, bindlessSet, getLightingSet ? getLightingSet() : VK_NULL_HANDLE);
			        renderQueue.Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
		        });
	}
} // namespace aether
