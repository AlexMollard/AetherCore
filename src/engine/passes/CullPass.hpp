#pragma once

#include <string>
#include "utils/Expected.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	class RenderGraph;
	class RenderQueue;

	// Registers a GPU compute pass that frustum-culls all DrawCommands submitted to a
	// RenderQueue and writes VkDrawIndexedIndirectCommand + per-batch draw counts into
	// device-local buffers. The paired ForwardPass then calls RenderQueue::FlushDraw to
	// issue DrawIndexedIndirectCount for each surviving batch.
	//
	// DrawCommands must be pre-populated into the queue by the game thread before
	// EndFrame (via AetherCore::GatherRenderDraws). This pass only dispatches the
	// compute cull shader - it no longer reads ECS directly.
	//
	// Usage:
	//   1. Initialize(device)          - once, at engine startup.
	//   2. RegisterPass(graph, …)      - once per render graph registration (main camera).
	//      Use namePrefix to disambiguate RTT variants, e.g. "RTT_0".
	//   3. Shutdown()                  - once, at engine shutdown.
	class CullPass
	{
	public:
		void Initialize(VkDevice device);
		void Shutdown();

		// Registers a "$CullDraws[_<namePrefix>]" compute pass that dispatches the
		// single-frustum cull shader against the pre-populated renderQueue.
		void RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix = {});

		[[nodiscard]] VkPipeline GetSinglePipeline() const
		{
			return m_singlePipeline;
		}

		[[nodiscard]] VkPipelineLayout GetSingleLayout() const
		{
			return m_singleLayout;
		}

		[[nodiscard]] VkPipeline GetMultiPipeline() const
		{
			return m_multiPipeline;
		}

		[[nodiscard]] VkPipelineLayout GetMultiLayout() const
		{
			return m_multiLayout;
		}

	private:
		Expected<void> EnsureSinglePipeline();
		Expected<void> EnsureMultiPipeline();

		VkDevice m_device = VK_NULL_HANDLE;
		VkPipeline m_singlePipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_singleLayout = VK_NULL_HANDLE;
		VkPipeline m_multiPipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_multiLayout = VK_NULL_HANDLE;
	};
} // namespace aether
