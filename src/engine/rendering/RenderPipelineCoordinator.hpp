#pragma once

#include <functional>
#include "vulkan/volk.hpp"

#include "rendering/RenderGraph.hpp"

namespace aether
{
	class BindlessManager;
	class CullPass;
	class ForwardPass;
	class PostProcessStack;
	class RenderQueue;
	class RenderTargetService;
	class ShadowService;
	class SkyboxPass;

	// Centralizes render-graph pass topology registration order.
	class RenderPipelineCoordinator
	{
	public:
		void RegisterPasses(RenderGraph& graph,
		        SkyboxPass& skyboxPass,
		        PostProcessStack& postProcessStack,
		        ShadowService& shadowService,
		        BindlessManager& bindlessManager,
		        VkDevice device,
		        VkFormat depthFormat,
		        CullPass& cullPass,
		        RenderQueue& mainRenderQueue,
		        ForwardPass& forwardPass,
		        std::function<VkDescriptorSet()> getLightingSet,
		        RenderTargetService& renderTargetService);
	};
} // namespace aether
