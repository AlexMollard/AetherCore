#include "material/BindlessContract.hpp"

#include <format>

#include "gpu/GpuDeviceFactory.hpp"
#include "utils/Assert.hpp"

namespace aether::bindless
{
	std::vector<gpu::DescriptorSetLayout> ComposePipelineSetLayouts(std::span<const gpu::DescriptorSetLayout> pipelineLayouts, gpu::DescriptorSetLayout bindlessLayout, const std::uint32_t bindlessSetIndex)
	{
		AE_ASSERT_ALWAYS(bindlessLayout != nullptr, "Bindless descriptor set layout is null.");

		const std::size_t requiredCount = static_cast<std::size_t>(bindlessSetIndex) + 1;
		const std::size_t outputCount = std::max(requiredCount, pipelineLayouts.size());

		std::vector<gpu::DescriptorSetLayout> out(outputCount, nullptr);
		for (std::size_t i = 0; i < pipelineLayouts.size(); ++i)
		{
			out[i] = pipelineLayouts[i];
		}

		out[bindlessSetIndex] = bindlessLayout;
		return out;
	}

	Expected<gpu::PipelineLayout> CreatePipelineLayoutWithBindless(
	        gpu::Device device, std::span<const gpu::DescriptorSetLayout> pipelineLayouts, gpu::DescriptorSetLayout bindlessLayout, std::span<const gpu::PushConstantRange> pushConstantRanges, const std::uint32_t bindlessSetIndex)
	{
		AE_ASSERT_ALWAYS(device != nullptr, "Cannot create pipeline layout: device is null.");

		const auto setLayouts = ComposePipelineSetLayouts(pipelineLayouts, bindlessLayout, bindlessSetIndex);
		const gpu::Factory::PipelineLayoutDesc desc{
		        .setLayouts = setLayouts,
		        .pushConstantRanges = pushConstantRanges,
		};
		return gpu::Factory::CreatePipelineLayout(device, desc);
	}
} // namespace aether::bindless
