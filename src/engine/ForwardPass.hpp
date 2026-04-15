#pragma once

#include <functional>
#include <vulkan/vulkan.h>

#include "RenderGraph.hpp"

namespace aether
{
	class RenderQueue;
	class Scene;
	class World;

	// Registers the engine's main forward scene pass.
	//
	// This pass does not own pipelines itself; it owns the render-graph policy
	// for the main scene submission stage: which attachments are used, how they
	// are loaded/cleared, and when scene/world draw queues are flushed.
	class ForwardPass
	{
	public:
		void RegisterPass(RenderGraph& graph, RGImage hdrColor, RGImage depth, Scene& scene, World& world, RenderQueue& renderQueue, VkDescriptorSet bindlessSet, std::function<VkDescriptorSet()> getLightingSet);
	};
} // namespace aether
