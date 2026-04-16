#pragma once

#include <string>
#include <vulkan/vulkan.h>

namespace aether
{
	class RenderGraph;
	class RenderQueue;
	class Scene;
	class World;

	// Registers a GPU compute pass that frustum-culls all DrawCommands submitted to a
	// RenderQueue and writes VkDrawIndexedIndirectCommand + per-batch draw counts into
	// device-local buffers. The paired ForwardPass then calls RenderQueue::FlushDraw to
	// issue DrawIndexedIndirectCount for each surviving batch.
	//
	// Usage:
	//   1. Initialize(device)          — once, at engine startup.
	//   2. RegisterPass(graph, …)      — once per render graph registration (main camera).
	//      Use namePrefix to disambiguate RTT variants, e.g. "RTT_0".
	//   3. Shutdown()                  — once, at engine shutdown.
	class CullPass
	{
	public:
		void Initialize(VkDevice device);
		void Shutdown();

		// Registers a "$CullDraws[_<namePrefix>]" compute pass that flushes scene + world
		// into renderQueue, then dispatches the frustum-cull compute shader.
		// Must be registered before the matching ForwardPass in the RenderGraph.
		void RegisterPass(RenderGraph& graph, Scene& scene, World& world, RenderQueue& renderQueue, const std::string& namePrefix = {});

		[[nodiscard]] VkPipeline GetPipeline() const
		{
			return m_pipeline;
		}

		[[nodiscard]] VkPipelineLayout GetPipelineLayout() const
		{
			return m_pipelineLayout;
		}

	private:
		void EnsurePipeline();

		VkDevice m_device = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
	};
} // namespace aether
