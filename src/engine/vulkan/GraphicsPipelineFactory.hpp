#pragma once

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether
{
	// ─────────────────────────────────────────────────────────────────────────
	// GraphicsPipelineFactory - Vulkan implementation of GraphicsPipeline::Create
	// ─────────────────────────────────────────────────────────────────────────
	// Lives under src/engine/vulkan/ so the engine-facing rendering/GraphicsPipeline
	// translation unit can stay Vulkan-free. Builds the pipeline + 4 GPL
	// libraries + pipeline layout + linked final pipeline, and returns a
	// ResourceRegistry::PipelineEntry populated with all 5 VkPipelines and the
	// VkPipelineLayout. The caller hands the entry to
	// aether::ResourceRegistry::RegisterPipeline to get a PipelineHandle; the
	// registry owns the deferred-destruction path for all 5 pipelines + the
	// layout.
	namespace vkutil
	{
		[[nodiscard]] Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device device, gpu::PipelineCache pipelineCache, const GraphicsPipeline::Desc& desc) noexcept;
	} // namespace vkutil
} // namespace aether
