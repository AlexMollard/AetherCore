#include "ForwardPass.hpp"

#include "RenderQueue.hpp"
#include "Scene.hpp"
#include "World.hpp"

namespace aether
{
	void ForwardPass::RegisterPass(RenderGraph& graph, RGImage hdrColor, RGImage depth, Scene& scene, World& world, RenderQueue& renderQueue, VkDescriptorSet bindlessSet, std::function<VkDescriptorSet()> getLightingSet)
	{
		graph.AddPass("$EngineForward")
		        .WriteColor(hdrColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .WriteDepth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, ClearDepthValue(1.0f))
		        .Execute(
		                [&scene, &world, &renderQueue, bindlessSet, getLightingSet](PassContext& ctx)
		                {
			                scene.FlushToQueue(renderQueue);
			                world.FlushToQueue(renderQueue);
			                renderQueue.Flush(ctx.recorder, ctx.frameConstantsAddr, bindlessSet, getLightingSet ? getLightingSet() : VK_NULL_HANDLE);
			                renderQueue.Clear();
		                });
	}
} // namespace aether
