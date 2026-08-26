#include "vulkan/ComputePipelineFactory.hpp"

#include <string>

#include "io/FileSystem.hpp"
#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/GlobalBindingLayout.hpp"
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

		// See GraphicsPipelineFactory: layout-free on the descriptor-heap path, real set layout
		// and push-constant range on the descriptor-buffer path. Same SPIR-V either way.
		const auto& global = vulkan::GetGlobalBindingLayout();
		// Keyed off the presence of a global layout, NOT off whether mappings were supplied:
		// some pipelines pass no mappings even on the heap path, and treating those as
		// "has a layout" hands vkCreateShadersEXT a null set layout.
		const bool layoutFree = global.pipelineLayout == VK_NULL_HANDLE;

		const VkDescriptorSetLayout setLayouts[] = {global.setLayout};
		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_ALL,
		        .offset = 0,
		        .size = global.pushConstantSize,
		};

		const VkShaderCreateInfoEXT createInfo{
		        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
		        .pNext = mappings,
		        .flags = layoutFree ? VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT : static_cast<VkShaderCreateFlagsEXT>(0),
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .nextStage = 0,
		        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
		        .codeSize = spirv->size(),
		        .pCode = spirv->data(),
		        .pName = entryName.c_str(),
		        .setLayoutCount = layoutFree ? 0u : 1u,
		        .pSetLayouts = layoutFree ? nullptr : setLayouts,
		        .pushConstantRangeCount = layoutFree ? 0u : 1u,
		        .pPushConstantRanges = layoutFree ? nullptr : &pushRange,
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
