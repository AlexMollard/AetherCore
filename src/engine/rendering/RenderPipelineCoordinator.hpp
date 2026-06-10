#pragma once

#include <functional>
#include "vulkan/volk.hpp"

#include "gpu/CommandList.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{
	class BindlessManager;
	class CullPass;
	class ForwardPass;
	class LocalShadowService;
	class PhysicsDebugRenderer;
	class PostProcessStack;
	class RenderQueue;
	class RenderTargetService;
	class ShadowService;
	class SkyboxPass;

	// Aggregates all dependencies needed for render graph pass registration.
	struct PassRegistrationContext
	{
		RenderGraph& graph;
		SkyboxPass& skyboxPass;
		PostProcessStack& postProcessStack;
		ShadowService& shadowService;
		LocalShadowService& localShadowService;
		BindlessManager& bindlessManager;
		VkDevice device = VK_NULL_HANDLE;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		CullPass& cullPass;
		RenderQueue& mainRenderQueue;
		ForwardPass& forwardPass;
		std::function<void(gpu::CommandList&, VkPipelineLayout)> pushLightingFn;
		RenderTargetService& renderTargetService;
		PhysicsDebugRenderer& physicsDebug;
	};

	// Centralizes render-graph pass topology registration order.
	class RenderPipelineCoordinator
	{
	public:
		void RegisterPasses(const PassRegistrationContext& ctx);
	};
} // namespace aether
