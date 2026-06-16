#pragma once

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether
{
	// Builds graphics pipelines + 4 GPL libraries + linked final pipeline. Returns a ResourceRegistry::PipelineEntry; the caller hands it to ResourceRegistry::RegisterPipeline.
	namespace vkutil
	{
		[[nodiscard]] Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device device, gpu::PipelineCache pipelineCache, const GraphicsPipeline::Desc& desc) noexcept;
	} // namespace vkutil
} // namespace aether
