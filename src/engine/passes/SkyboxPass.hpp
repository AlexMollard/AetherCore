#pragma once

#include "vulkan/volk.hpp"

#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{
	// Owns the pipeline for the engine's procedural sky pass.
	//
	// The pass runs before $EngineForward and clears the HDR colour buffer
	// with a gradient sky + sun disk.  Forward geometry then draws on top.
	//
	// Unlike PostProcessStack, SkyboxPass has no extent-dependent resources
	// (no offscreen images), so it only needs to be created once and survives
	// swapchain recreation intact.  Only RegisterPass() needs to be called
	// again each time the render graph is rebuilt.
	//
	// Lifetime contract:
	//   1. Create()        - allocates the Vulkan pipeline
	//   2. RegisterPass()  - adds "$Skybox" to the graph; called every rebuild
	//   3. Destroy()       - releases the pipeline; call before device shutdown
	class SkyboxPass
	{
	public:
		struct Desc
		{
			VkDevice device = VK_NULL_HANDLE;
			VkPipelineCache pipelineCache = VK_NULL_HANDLE;
			VkFormat hdrColorFormat = VK_FORMAT_UNDEFINED;
		};

		SkyboxPass() = default;

		SkyboxPass(const SkyboxPass&) = delete;
		SkyboxPass& operator=(const SkyboxPass&) = delete;

		SkyboxPass(SkyboxPass&&) noexcept = default;
		SkyboxPass& operator=(SkyboxPass&&) noexcept = default;

		static SkyboxPass Create(const Desc& desc);
		void Destroy();

		// Add the "$Skybox" pass to the render graph.
		// Must be called each time the graph is rebuilt, before "$EngineForward".
		// hdrColor - the HDR offscreen buffer this pass will clear and fill.
		void RegisterPass(RenderGraph& graph, RGImage hdrColor);

	private:
		GraphicsPipeline m_pipeline;
	};
} // namespace aether
