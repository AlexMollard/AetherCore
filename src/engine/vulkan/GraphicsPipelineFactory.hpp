#pragma once

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether::vkutil
{
	[[nodiscard]] Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device device, const GraphicsPipeline::Desc& desc) noexcept;
}
