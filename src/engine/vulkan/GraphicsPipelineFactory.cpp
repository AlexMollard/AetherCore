#include "vulkan/GraphicsPipelineFactory.hpp"

#include <string>
#include <vector>

#include "io/FileSystem.hpp"
#include "utils/Assert.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::vkutil
{
	// Builds graphics VkShaderEXT handles via vkCreateShadersEXT. Vertex and
	// fragment shaders are linked (VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) and
	// tagged VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT so they consume push data
	// via vkCmdPushDataEXT. No VkPipeline / VkPipelineLayout is created - all
	// fixed-function state (topology, rasterizer, depth, blend) is cached on
	// the returned PipelineEntry and re-applied through vkCmdSet* on each bind.
	Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device gpuDevice, const GraphicsPipeline::Desc& desc) noexcept
	{
		auto device = static_cast<VkDevice>(gpuDevice);
		const VkFormat vkColorFormat = gpu::ToVk(desc.colorFormat);
		const VkCompareOp vkDepthCompareOp = gpu::ToVk(desc.depthCompareOp);
		const bool hasColorAttachment = vkColorFormat != VK_FORMAT_UNDEFINED;

		// Load vertex SPIR-V.
		AE_TRY(vertSpirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (vertSpirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("GraphicsPipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

		// Load fragment SPIR-V (separate file when provided, otherwise the same module).
		std::vector<std::byte> fragSpirvStorage;
		const std::vector<std::byte>* fragSpirv = &*vertSpirv;
		const bool hasSeparateFragment = !desc.fragmentVfsPath.empty();
		if (hasSeparateFragment)
		{
			AE_TRY(fragLoaded, io::FileSystem::ReadFile(desc.fragmentVfsPath));
			if (fragLoaded->empty())
			{
				AE_UNEXPECTED(AetherError::Asset("GraphicsPipeline: fragment shader not found: " + std::string(desc.fragmentVfsPath)));
			}
			fragSpirvStorage = std::move(*fragLoaded);
			fragSpirv = &fragSpirvStorage;
		}

		const std::string vertEntry(desc.vertexEntry);
		const std::string fragEntry(desc.fragmentEntry);

		const auto* mappings = static_cast<const VkShaderDescriptorSetAndBindingMappingInfoEXT*>(desc.descriptorHeapMappings);

		// Shader-object create flags: layout-free (descriptor heap) + link the
		// two stages so the driver can cross-optimize vert/frag.
		const VkShaderCreateFlagsEXT shaderFlags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;

		VkShaderCreateInfoEXT vertInfo{
		        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
		        .pNext = mappings,
		        .flags = shaderFlags,
		        .stage = VK_SHADER_STAGE_VERTEX_BIT,
		        .nextStage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
		        .codeSize = vertSpirv->size(),
		        .pCode = vertSpirv->data(),
		        .pName = vertEntry.c_str(),
		        .setLayoutCount = 0,
		        .pSetLayouts = nullptr,
		        .pushConstantRangeCount = 0,
		        .pPushConstantRanges = nullptr,
		        .pSpecializationInfo = nullptr,
		};

		VkShaderCreateInfoEXT fragInfo{
		        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
		        .pNext = mappings,
		        .flags = shaderFlags,
		        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .nextStage = 0,
		        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
		        .codeSize = fragSpirv->size(),
		        .pCode = fragSpirv->data(),
		        .pName = fragEntry.c_str(),
		        .setLayoutCount = 0,
		        .pSetLayouts = nullptr,
		        .pushConstantRangeCount = 0,
		        .pPushConstantRanges = nullptr,
		        .pSpecializationInfo = nullptr,
		};

		const VkShaderCreateInfoEXT shaderCreateInfos[] = {vertInfo, fragInfo};
		VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		VkShaderEXT& vertShader = shaders[0];
		VkShaderEXT& fragShader = shaders[1];

		const VkResult result = vkCreateShadersEXT(device, 2, shaderCreateInfos, nullptr, shaders);
		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create graphics shaders for " + std::string(desc.shaderVfsPath)));
		}

		// Name the shaders for debugging.
		if (desc.debugName != nullptr)
		{
			const std::string vertName = std::string(desc.debugName) + ".vert";
			const std::string fragName = std::string(desc.debugName) + ".frag";
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(vertShader), VK_OBJECT_TYPE_SHADER_EXT, vertName.c_str());
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(fragShader), VK_OBJECT_TYPE_SHADER_EXT, fragName.c_str());
		}
		else
		{
			const std::string vertName = std::string(desc.shaderVfsPath) + ".vert";
			const std::string fragName = (hasSeparateFragment ? std::string(desc.fragmentVfsPath) : std::string(desc.shaderVfsPath)) + ".frag";
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(vertShader), VK_OBJECT_TYPE_SHADER_EXT, vertName.c_str());
			vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(fragShader), VK_OBJECT_TYPE_SHADER_EXT, fragName.c_str());
		}

		// -- Build the entry with cached dynamic state -----------------------
		ResourceRegistry::PipelineEntry entry{};
		entry.device = device;
		entry.isGraphics = true;
		entry.vertexShader = vertShader;
		entry.fragmentShader = fragShader;

		entry.topology = gpu::ToVk(desc.topology);
		entry.polygonMode = gpu::ToVk(desc.polygonMode);
		entry.cullMode = VK_CULL_MODE_NONE;
		entry.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		entry.depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE;
		entry.depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE;
		entry.depthCompareOp = vkDepthCompareOp;
		entry.rasterizationSampleCount = VK_SAMPLE_COUNT_1_BIT;
		entry.lineWidth = 1.0f;
		entry.hasLineWidth = desc.lineWidthDynamic;

		// Color blend (single attachment - the engine never uses MRT).
		if (hasColorAttachment)
		{
			entry.colorBlendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
			entry.colorBlendEquation.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			entry.colorBlendEquation.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			entry.colorBlendEquation.colorBlendOp = VK_BLEND_OP_ADD;
			entry.colorBlendEquation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			entry.colorBlendEquation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			entry.colorBlendEquation.alphaBlendOp = VK_BLEND_OP_ADD;
			entry.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		}
		else
		{
			entry.colorBlendEnable = VK_FALSE;
			entry.colorWriteMask = 0;
		}

		// Vertex input (dynamic). Empty for BDA-only pipelines.
		if (!desc.vertexBindings.empty())
		{
			entry.vertexBindings.reserve(desc.vertexBindings.size());
			for (const gpu::VertexInputBinding& b: desc.vertexBindings)
			{
				entry.vertexBindings.push_back(VkVertexInputBindingDescription2EXT{
				        .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
				        .pNext = nullptr,
				        .binding = b.binding,
				        .stride = b.stride,
				        .inputRate = static_cast<VkVertexInputRate>(b.inputRate),
				        .divisor = 1,
				});
			}
		}
		if (!desc.vertexAttributes.empty())
		{
			entry.vertexAttributes.reserve(desc.vertexAttributes.size());
			for (const gpu::VertexInputAttribute& a: desc.vertexAttributes)
			{
				entry.vertexAttributes.push_back(VkVertexInputAttributeDescription2EXT{
				        .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
				        .pNext = nullptr,
				        .location = a.location,
				        .binding = a.binding,
				        .format = gpu::ToVk(a.format),
				        .offset = a.offset,
				});
			}
		}

		return entry;
	}
} // namespace aether::vkutil
