#include "vulkan/ComputePipelineFactory.hpp"

#include <string>

#include "io/FileSystem.hpp"
#include "utils/Assert.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::vkutil
{
	Expected<ResourceRegistry::PipelineEntry> CreateComputePipelineEntry(gpu::Device gpuDevice, gpu::PipelineCache gpuPipelineCache, const ComputePipelineDesc& desc) noexcept
	{
		const auto device = static_cast<VkDevice>(gpuDevice);
		const auto pipelineCache = static_cast<VkPipelineCache>(gpuPipelineCache);

		AE_TRY(spirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("ComputePipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

		const std::string owner = desc.debugName ? desc.debugName : "ComputePipeline";
		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, *spirv, owner.c_str()));

		VkPipelineLayout vkLayout = desc.existingLayout;
		if (vkLayout == VK_NULL_HANDLE)
		{
			VkPushConstantRange pushRange{};
			VkPipelineLayoutCreateInfo layoutInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			};
			if (desc.pushConstantSize > 0)
			{
				pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				pushRange.offset = 0;
				pushRange.size = desc.pushConstantSize;
				layoutInfo.pushConstantRangeCount = 1;
				layoutInfo.pPushConstantRanges = &pushRange;
			}
			if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &vkLayout) != VK_SUCCESS)
			{
				vkDestroyShaderModule(device, shaderModule, nullptr);
				AE_UNEXPECTED(AetherError::Vulkan(0, "ComputePipeline: failed to create pipeline layout for " + owner));
			}
		}

		const std::string entry(desc.shaderEntry);
		const VkPipelineShaderStageCreateInfo stage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule,
		        .pName = entry.c_str(),
		};
		VkPipeline vkPipeline = VK_NULL_HANDLE;
		const VkComputePipelineCreateInfo pipelineInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .stage = stage,
		        .layout = vkLayout,
		};
		const VkResult result = vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &vkPipeline);
		vkDestroyShaderModule(device, shaderModule, nullptr);

		if (result != VK_SUCCESS)
		{
			vkDestroyPipelineLayout(device, vkLayout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "ComputePipeline: failed to create pipeline for " + owner));
		}

		if (desc.debugName != nullptr)
		{
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(vkPipeline), VK_OBJECT_TYPE_PIPELINE, desc.debugName);
		}

		ResourceRegistry::PipelineEntry entryOut{};
		entryOut.pipeline = vkPipeline;
		entryOut.layout = vkLayout;
		entryOut.device = device;
		entryOut.ownsLayout = (desc.existingLayout == VK_NULL_HANDLE);
		return entryOut;
	}
} // namespace aether::vkutil
