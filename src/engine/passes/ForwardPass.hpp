#pragma once

#include <functional>
#include <span>
#include "vulkan/volk.hpp"

#include "rendering/RenderGraph.hpp"

namespace aether
{
	class RenderQueue;

	// Registers the engine's main forward scene pass.
	//
	// Draws the commands produced by the preceding CullPass compute pass using
	// DrawIndexedIndirectCount. Scene and world flushing happens in CullPass - this
	// pass only records draw calls and clears the queue.
	class ForwardPass
	{
	public:
		static void SetEnabled(bool enabled)
		{
			s_enabled = enabled;
		}

		[[nodiscard]] static bool IsEnabled()
		{
			return s_enabled;
		}

		void RegisterPass(RenderGraph& graph,
		        RGImage hdrColor,
		        RGImage depth,
		        RenderQueue& renderQueue,
		        VkDescriptorSet bindlessSet,
		        std::function<void(VkCommandBuffer, VkPipelineLayout)> pushLightingFn,
		        std::span<const RGImage> shadowMaps = {},
		        RGImage localShadowAtlas = {});

	private:
		static bool s_enabled;
	};
} // namespace aether
