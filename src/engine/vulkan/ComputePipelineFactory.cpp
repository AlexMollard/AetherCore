#include "vulkan/ComputePipelineFactory.hpp"

#include <string>

#include "io/FileSystem.hpp"
#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/VulkanUtils.hpp"
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
#	include "vulkan/AftermathContext.hpp"
#endif

namespace aether::vkutil
{
	// layout-free (VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT) - all push data
	Expected<ResourceRegistry::PipelineEntry> CreateComputePipelineEntry(gpu::Device gpuDevice, const ComputePipelineDesc& desc) noexcept
	{
		AE_PROFILE_ZONE();
		auto* device = static_cast<VkDevice>(gpuDevice);

		AE_TRY(spirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("ComputePipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

		const std::string owner = desc.debugName ? desc.debugName : "ComputePipeline";
		const std::string entryName(desc.shaderEntry);

		const auto* mappings = static_cast<const VkShaderDescriptorSetAndBindingMappingInfoEXT*>(desc.descriptorHeapMappings);

		const VkShaderCreateInfoEXT createInfo{
		        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
		        .pNext = mappings,
		        .flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .nextStage = 0,
		        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
		        .codeSize = spirv->size(),
		        .pCode = spirv->data(),
		        .pName = entryName.c_str(),
		        .setLayoutCount = 0,
		        .pSetLayouts = nullptr,
		        .pushConstantRangeCount = 0,
		        .pPushConstantRanges = nullptr,
		        .pSpecializationInfo = nullptr,
		};

		VkShaderEXT shader = VK_NULL_HANDLE;
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		AftermathContext::RegisterShaderBinary(spirv->data(), static_cast<uint32_t>(spirv->size()));
#endif
		const VkResult result = vkCreateShadersEXT(device, 1, &createInfo, nullptr, &shader);

		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "ComputePipeline: failed to create shader for " + owner));
		}

		if (desc.debugName != nullptr)
		{
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(shader), VK_OBJECT_TYPE_SHADER_EXT, desc.debugName);
		}

		ResourceRegistry::PipelineEntry entryOut{};
		entryOut.device = device;
		entryOut.computeShader = shader;
		entryOut.isGraphics = false;
		return entryOut;
	}
} // namespace aether::vkutil
