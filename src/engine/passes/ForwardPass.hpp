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
		void RegisterPass(RenderGraph& graph, RGImage hdrColor, RGImage depth, RenderQueue& renderQueue, VkDescriptorSet bindlessSet, std::function<VkDescriptorSet()> getLightingSet, std::span<const RGImage> shadowMaps = {}, RGImage localShadowAtlas = {});
	};
} // namespace aether
