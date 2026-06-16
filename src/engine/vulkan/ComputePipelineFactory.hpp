#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether::vkutil
{
	// Builds compute pipelines. Produces a ResourceRegistry::PipelineEntry; the caller hands it to ResourceRegistry::RegisterPipeline for handle + deferred destruction.
	struct ComputePipelineDesc
	{
		const char* shaderVfsPath = nullptr;
		const char* shaderEntry = "main";
		std::uint32_t pushConstantSize = 0;
		const char* debugName = nullptr;
		VkPipelineLayout existingLayout = VK_NULL_HANDLE;
	};

	[[nodiscard]] Expected<ResourceRegistry::PipelineEntry> CreateComputePipelineEntry(gpu::Device device, gpu::PipelineCache pipelineCache, const ComputePipelineDesc& desc) noexcept;
} // namespace aether::vkutil
