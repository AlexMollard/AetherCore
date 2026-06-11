#pragma once

#include <functional>
#include "vulkan/volk.hpp"

#include "gpu/CommandList.hpp"
#include "rendering/FrameContext.hpp"
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
	// Wraps a stable FrameContext (runtime references + formats) and adds the
	// per-registration extras that are not part of every-frame state: the
	// individual pass services (skybox, shadows, post-process, RTT) and the
	// lighting push lambda rebuilt each RegisterPasses() call.
	struct PassRegistrationContext
	{
		FrameContext frame;
		VkDevice device = VK_NULL_HANDLE;
		SkyboxPass& skyboxPass;
		PostProcessStack& postProcessStack;
		ShadowService& shadowService;
		LocalShadowService& localShadowService;
		CullPass& cullPass;
		RenderQueue& mainRenderQueue;
		ForwardPass& forwardPass;
		std::function<void(gpu::CommandList&, gpu::PipelineLayout)> pushLightingFn;
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
