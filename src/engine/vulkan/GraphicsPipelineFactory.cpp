#include "vulkan/GraphicsPipelineFactory.hpp"

#include <string>
#include <vector>

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
	Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device gpuDevice, const GraphicsPipeline::Desc& desc) noexcept
	{
		AE_PROFILE_ZONE();
		auto* device = static_cast<VkDevice>(gpuDevice);
		const VkFormat vkColorFormat = gpu::ToVk(desc.colorFormat);
		const VkCompareOp vkDepthCompareOp = gpu::ToVk(desc.depthCompareOp);
		const bool hasColorAttachment = vkColorFormat != VK_FORMAT_UNDEFINED;

		AE_TRY(vertSpirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (vertSpirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("GraphicsPipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

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

		// Two ways to give a shader object its bindings. With VK_EXT_descriptor_heap the shader
		// is created layout-free and the mappings redirect its set-0 bindings onto the heap.
		// Without it, the identical SPIR-V is handed the real set layout and push-constant
		// range instead - shader objects take those directly, so there is still no VkPipeline.
		const auto& global = vulkan::GetGlobalBindingLayout();
		// Keyed off the presence of a global layout, NOT off whether mappings were supplied:
		// some pipelines pass no mappings even on the heap path, and treating those as
		// "has a layout" hands vkCreateShadersEXT a null set layout.
		const bool layoutFree = global.pipelineLayout == VK_NULL_HANDLE;

		VkShaderCreateFlagsEXT shaderFlags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
		if (layoutFree)
		{
			shaderFlags |= VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
		}

		const VkDescriptorSetLayout setLayouts[] = {global.setLayout};
		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_ALL,
		        .offset = 0,
		        .size = global.pushConstantSize,
		};
		const std::uint32_t setLayoutCount = layoutFree ? 0u : 1u;
		const std::uint32_t pushRangeCount = layoutFree ? 0u : 1u;

		const VkShaderCreateInfoEXT vertInfo{
		        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
		        .pNext = mappings,
		        .flags = shaderFlags,
		        .stage = VK_SHADER_STAGE_VERTEX_BIT,
		        .nextStage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
		        .codeSize = vertSpirv->size(),
		        .pCode = vertSpirv->data(),
		        .pName = vertEntry.c_str(),
		        .setLayoutCount = setLayoutCount,
		        .pSetLayouts = layoutFree ? nullptr : setLayouts,
		        .pushConstantRangeCount = pushRangeCount,
		        .pPushConstantRanges = layoutFree ? nullptr : &pushRange,
		        .pSpecializationInfo = nullptr,
		};

		const VkShaderCreateInfoEXT fragInfo{
		        .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
		        .pNext = mappings,
		        .flags = shaderFlags,
		        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .nextStage = 0,
		        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
		        .codeSize = fragSpirv->size(),
		        .pCode = fragSpirv->data(),
		        .pName = fragEntry.c_str(),
		        .setLayoutCount = setLayoutCount,
		        .pSetLayouts = layoutFree ? nullptr : setLayouts,
		        .pushConstantRangeCount = pushRangeCount,
		        .pPushConstantRanges = layoutFree ? nullptr : &pushRange,
		        .pSpecializationInfo = nullptr,
		};

		const VkShaderCreateInfoEXT shaderCreateInfos[] = {vertInfo, fragInfo};
		VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		VkShaderEXT& vertShader = shaders[0];
		VkShaderEXT& fragShader = shaders[1];

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		AftermathContext::RegisterShaderBinary(vertSpirv->data(), static_cast<uint32_t>(vertSpirv->size()));
		if (hasSeparateFragment)
		{
			AftermathContext::RegisterShaderBinary(fragSpirv->data(), static_cast<uint32_t>(fragSpirv->size()));
		}
#endif
		const VkResult result = vkCreateShadersEXT(device, 2, shaderCreateInfos, nullptr, shaders);
		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create graphics shaders for " + std::string(desc.shaderVfsPath)));
		}

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

		ResourceRegistry::PipelineEntry entry{};
		entry.device = device;
		entry.isGraphics = true;
		entry.vertexShader = vertShader;
		entry.fragmentShader = fragShader;

		entry.topology = gpu::ToVk(desc.topology);
		entry.polygonMode = gpu::ToVk(desc.polygonMode);
		entry.cullMode = gpu::ToVk(desc.cullMode);
		entry.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		entry.depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE;
		entry.depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE;
		entry.depthCompareOp = vkDepthCompareOp;
		entry.rasterizationSampleCount = VK_SAMPLE_COUNT_1_BIT;
		entry.lineWidth = 1.0f;
		entry.hasLineWidth = desc.lineWidthDynamic;

		// Colour blend. Every attachment gets the same state - the one pipeline that
		// writes more than one (the depth prepass) wants both written opaquely - but the
		// count has to be right, because the dynamic-state calls below only cover the
		// attachments they are told about and the rest are left undefined.
		const std::uint32_t colorAttachments = hasColorAttachment
		        ? (std::min) (
		                  (std::max) (desc.colorAttachmentCount, 1u),
		                  ResourceRegistry::PipelineEntry::kMaxColorAttachments)
		        : 0u;
		entry.colorAttachmentCount = colorAttachments;
		for (std::uint32_t i = 0; i < colorAttachments; ++i)
		{
			entry.colorBlendEnable[i] = desc.blendEnable && desc.blendMode != gpu::BlendMode::Opaque ? VK_TRUE : VK_FALSE;
			entry.colorBlendEquation[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			entry.colorBlendEquation[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			if (desc.blendMode == gpu::BlendMode::Additive)
			{
				entry.colorBlendEquation[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				entry.colorBlendEquation[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			}
			else if (desc.blendMode == gpu::BlendMode::Multiply)
			{
				entry.colorBlendEquation[i].srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
				entry.colorBlendEquation[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
			}
			else if (desc.blendMode == gpu::BlendMode::Premultiplied)
			{
				entry.colorBlendEquation[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				entry.colorBlendEquation[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			}
			entry.colorBlendEquation[i].colorBlendOp = VK_BLEND_OP_ADD;
			entry.colorBlendEquation[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			entry.colorBlendEquation[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			entry.colorBlendEquation[i].alphaBlendOp = VK_BLEND_OP_ADD;
			entry.colorWriteMask[i] = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		}
		if (colorAttachments == 0)
		{
			// Still one element of state, all writes masked off, so a depth-only pipeline
			// leaves nothing enabled behind it.
			entry.colorAttachmentCount = 1;
			entry.colorBlendEnable[0] = VK_FALSE;
			entry.colorWriteMask[0] = 0;
		}

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
