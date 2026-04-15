#include "BindlessContract.hpp"

#include <format>

#include "AetherExceptions.hpp"

namespace aether::bindless
{
	std::vector<VkDescriptorSetLayout> ComposePipelineSetLayouts(std::span<const VkDescriptorSetLayout> pipelineLayouts, VkDescriptorSetLayout bindlessLayout, const std::uint32_t bindlessSetIndex)
	{
		if (bindlessLayout == VK_NULL_HANDLE)
		{
			throw VulkanError("Bindless descriptor set layout is null.");
		}

		const std::size_t requiredCount = static_cast<std::size_t>(bindlessSetIndex) + 1;
		const std::size_t outputCount = std::max(requiredCount, pipelineLayouts.size());

		std::vector<VkDescriptorSetLayout> out(outputCount, VK_NULL_HANDLE);
		for (std::size_t i = 0; i < pipelineLayouts.size(); ++i)
		{
			out[i] = pipelineLayouts[i];
		}

		out[bindlessSetIndex] = bindlessLayout;
		return out;
	}

	VkPipelineLayout CreatePipelineLayoutWithBindless(VkDevice device, std::span<const VkDescriptorSetLayout> pipelineLayouts, VkDescriptorSetLayout bindlessLayout, std::span<const VkPushConstantRange> pushConstantRanges, const std::uint32_t bindlessSetIndex)
	{
		if (device == VK_NULL_HANDLE)
		{
			throw VulkanError("Cannot create pipeline layout: VkDevice is null.");
		}

		const auto setLayouts = ComposePipelineSetLayouts(pipelineLayouts, bindlessLayout, bindlessSetIndex);
		const VkPipelineLayoutCreateInfo createInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size()),
			.pSetLayouts = setLayouts.data(),
			.pushConstantRangeCount = static_cast<std::uint32_t>(pushConstantRanges.size()),
			.pPushConstantRanges = pushConstantRanges.data(),
		};

		VkPipelineLayout layout = VK_NULL_HANDLE;
		const VkResult result = vkCreatePipelineLayout(device, &createInfo, nullptr, &layout);
		if (result != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to create bindless-aware pipeline layout. VkResult={}", static_cast<int>(result)));
		}

		return layout;
	}
} // namespace aether::bindless
