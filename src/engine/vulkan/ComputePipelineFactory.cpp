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
		auto device = static_cast<VkDevice>(gpuDevice);
		auto pipelineCache = static_cast<VkPipelineCache>(gpuPipelineCache);

		AE_TRY(spirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("ComputePipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

		const std::string owner = desc.debugName ? desc.debugName : "ComputePipeline";
		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, *spirv, owner.c_str()));

		const auto* mappings = static_cast<const VkShaderDescriptorSetAndBindingMappingInfoEXT*>(desc.descriptorHeapMappings);

		// All compute pipelines use VK_EXT_descriptor_heap for push constants
		// (vkCmdPushDataEXT) and access all data via BDA — no VkPipelineLayout
		// or VkPushConstantRange is needed.
		const VkPipelineLayout vkLayout = VK_NULL_HANDLE;

		const std::string entry(desc.shaderEntry);
		VkPipelineShaderStageCreateInfo stage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule.Get(),
		        .pName = entry.c_str(),
		};
		if (mappings)
		{
			stage.pNext = const_cast<VkShaderDescriptorSetAndBindingMappingInfoEXT*>(mappings);
		}
		VkPipelineCreateFlags2CreateInfo flags2{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
		};
		VkPipeline vkPipeline = VK_NULL_HANDLE;
		VkComputePipelineCreateInfo pipelineInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .pNext = &flags2,
		        .stage = stage,
		        .layout = vkLayout,
		};
		const VkResult result = vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &vkPipeline);
		// shaderModule RAII-destroys here regardless of result.

		if (result != VK_SUCCESS)
		{
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
		entryOut.ownsLayout = false;
		return entryOut;
	}
} // namespace aether::vkutil
