#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"

namespace aether::bindless
{
	inline constexpr std::uint32_t kDescriptorSetIndex = 1;
	inline constexpr std::uint32_t kSampledImageBinding = 0;
	inline constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;
	// Engine-side push-constant stages. Mirrors the old VK_SHADER_STAGE_ALL
	// convenience constant: vertex | fragment | compute.
	inline constexpr gpu::ShaderStage kDefaultStages = gpu::ShaderStage::All;

	[[nodiscard]] std::vector<gpu::DescriptorSetLayout> ComposePipelineSetLayouts(std::span<const gpu::DescriptorSetLayout> pipelineLayouts, gpu::DescriptorSetLayout bindlessLayout, std::uint32_t bindlessSetIndex = kDescriptorSetIndex);

	[[nodiscard]] Expected<gpu::PipelineLayout> CreatePipelineLayoutWithBindless(gpu::Device device,
	        std::span<const gpu::DescriptorSetLayout> pipelineLayouts,
	        gpu::DescriptorSetLayout bindlessLayout,
	        std::span<const gpu::PushConstantRange> pushConstantRanges = {},
	        std::uint32_t bindlessSetIndex = kDescriptorSetIndex);
} // namespace aether::bindless
