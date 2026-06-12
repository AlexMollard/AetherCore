#pragma once

#include <cstdint>

#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether::vkutil
{
	// -------------------------------------------------------------------------
	// ComputePipelineFactory - Vulkan implementation of compute-pipeline Create
	// -------------------------------------------------------------------------
	// Lives under src/engine/vulkan/ so engine-facing code (CullPass etc.) does
	// not need to know about vkCreateComputePipelines. The factory produces a
	// ResourceRegistry::PipelineEntry; the caller is responsible for handing
	// the entry to aether::ResourceRegistry::RegisterPipeline to get a stable
	// PipelineHandle for binding + deferred destruction.
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
