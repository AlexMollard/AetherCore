#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether::vkutil
{
	// Builds a compute VkShaderEXT. Produces a ResourceRegistry::PipelineEntry;
	// the caller hands it to ResourceRegistry::RegisterPipeline for handle + deferred destruction.
	struct ComputePipelineDesc
	{
		const char* shaderVfsPath = nullptr;
		const char* shaderEntry = "main";
		const char* debugName = nullptr;
		const void* descriptorHeapMappings = nullptr;
	};

	[[nodiscard]] Expected<ResourceRegistry::PipelineEntry> CreateComputePipelineEntry(gpu::Device device, const ComputePipelineDesc& desc) noexcept;
} // namespace aether::vkutil
