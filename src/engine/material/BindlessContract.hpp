#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include "utils/Expected.hpp"
#include "vulkan/volk.hpp"

namespace aether::bindless
{
	inline constexpr std::uint32_t kDescriptorSetIndex = 1;
	inline constexpr std::uint32_t kSampledImageBinding = 0;
	inline constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;
	inline constexpr VkShaderStageFlags kDefaultStages = VK_SHADER_STAGE_ALL;

	[[nodiscard]] std::vector<VkDescriptorSetLayout> ComposePipelineSetLayouts(std::span<const VkDescriptorSetLayout> pipelineLayouts, VkDescriptorSetLayout bindlessLayout, std::uint32_t bindlessSetIndex = kDescriptorSetIndex);

	[[nodiscard]] Expected<VkPipelineLayout> CreatePipelineLayoutWithBindless(VkDevice device, std::span<const VkDescriptorSetLayout> pipelineLayouts, VkDescriptorSetLayout bindlessLayout, std::span<const VkPushConstantRange> pushConstantRanges = {}, std::uint32_t bindlessSetIndex = kDescriptorSetIndex);
} // namespace aether::bindless
