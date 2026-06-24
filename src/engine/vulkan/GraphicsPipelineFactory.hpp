#pragma once

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

// Builds graphics pipelines + 4 GPL libraries + linked final pipeline. Returns a ResourceRegistry::PipelineEntry; the caller hands it to ResourceRegistry::RegisterPipeline.
namespace aether::vkutil
{
	[[nodiscard]] Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device device, const GraphicsPipeline::Desc& desc) noexcept;
} // namespace aether::vkutil
