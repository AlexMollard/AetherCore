#include "vulkan/GraphicsPipelineFactory.hpp"

#include <string>
#include <utility>
#include <vector>

#include "io/FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::vkutil
{
	Expected<ResourceRegistry::PipelineEntry> CreateGraphicsPipelineEntry(gpu::Device gpuDevice, gpu::PipelineCache gpuPipelineCache, const GraphicsPipeline::Desc& desc) noexcept
	{
		const VkDevice device = static_cast<VkDevice>(gpuDevice);
		const VkPipelineCache pipelineCache = static_cast<VkPipelineCache>(gpuPipelineCache);
		const VkFormat vkColorFormat = gpu::ToVk(desc.colorFormat);
		const VkFormat vkDepthFormat = gpu::ToVk(desc.depthFormat);
		const VkCompareOp vkDepthCompareOp = gpu::ToVk(desc.depthCompareOp);
		const VkShaderStageFlags vkPushConstantStages = gpu::ToVk(desc.pushConstantStages);
		const bool hasColorAttachment = vkColorFormat != VK_FORMAT_UNDEFINED;

		AE_TRY(spirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("GraphicsPipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

		AE_EXPECT_OR_THROW(vertModule, vkutil::CreateShaderModule(device, *spirv, "GraphicsPipeline"));

		VkShaderModule fragModule = VK_NULL_HANDLE;
		const bool hasSeparateFragment = !desc.fragmentVfsPath.empty();
		if (hasSeparateFragment)
		{
			AE_TRY(fragSpirv, io::FileSystem::ReadFile(desc.fragmentVfsPath));
			if (fragSpirv->empty())
			{
				vkDestroyShaderModule(device, vertModule, nullptr);
				AE_UNEXPECTED(AetherError::Asset("GraphicsPipeline: fragment shader not found: " + std::string(desc.fragmentVfsPath)));
			}
			{
				auto fragResult = vkutil::CreateShaderModule(device, *fragSpirv, "GraphicsPipeline.Fragment");
				AE_EXPECT_OR_THROW_VOID(fragResult);
				fragModule = std::move(*fragResult);
			}
		}
		else
		{
			fragModule = vertModule;
		}

		auto destroyModules = [&]()
		{
			if (hasSeparateFragment && fragModule != VK_NULL_HANDLE)
			{
				vkDestroyShaderModule(device, fragModule, nullptr);
			}
			vkDestroyShaderModule(device, vertModule, nullptr);
		};

		const std::string vertEntry(desc.vertexEntry);
		const std::string fragEntry(desc.fragmentEntry);

		// ── Vertex input (bindings + attributes) ────────────────────────────
		// Use caller-provided vertex bindings/attributes when non-empty, else
		// an empty vertex input (no vertex buffers needed).
		const bool hasVertexInput = !desc.vertexBindings.empty();
		std::vector<VkVertexInputBindingDescription> vkVkBindings;
		std::vector<VkVertexInputAttributeDescription> vkVkAttribs;
		if (hasVertexInput)
		{
			vkVkBindings.reserve(desc.vertexBindings.size());
			for (const gpu::VertexInputBinding& b: desc.vertexBindings)
			{
				vkVkBindings.push_back(VkVertexInputBindingDescription{
				        .binding = b.binding,
				        .stride = b.stride,
				        .inputRate = static_cast<VkVertexInputRate>(b.inputRate),
				});
			}
			vkVkAttribs.reserve(desc.vertexAttributes.size());
			for (const gpu::VertexInputAttribute& a: desc.vertexAttributes)
			{
				vkVkAttribs.push_back(VkVertexInputAttributeDescription{
				        .location = a.location,
				        .binding = a.binding,
				        .format = gpu::ToVk(a.format),
				        .offset = a.offset,
				});
			}
		}
		const VkPipelineVertexInputStateCreateInfo vertexInput{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		        .vertexBindingDescriptionCount = static_cast<std::uint32_t>(vkVkBindings.size()),
		        .pVertexBindingDescriptions = vkVkBindings.data(),
		        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vkVkAttribs.size()),
		        .pVertexAttributeDescriptions = vkVkAttribs.data(),
		};
		const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		        .topology = gpu::ToVk(desc.topology),
		};
		const VkPipelineViewportStateCreateInfo viewportState{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		        .viewportCount = 1,
		        .scissorCount = 1,
		};
		const VkPipelineRasterizationStateCreateInfo rasterizer{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		        .polygonMode = gpu::ToVk(desc.polygonMode),
		        .cullMode = VK_CULL_MODE_NONE,
		        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		        .lineWidth = 1.0f,
		};
		const VkPipelineMultisampleStateCreateInfo multisampling{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		const VkPipelineDepthStencilStateCreateInfo depthStencil{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		        .depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE,
		        .depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE,
		        .depthCompareOp = vkDepthCompareOp,
		        .depthBoundsTestEnable = VK_FALSE,
		        .stencilTestEnable = VK_FALSE,
		        .minDepthBounds = 0.0f,
		        .maxDepthBounds = 1.0f,
		};
		const VkPipelineColorBlendAttachmentState colorBlendAttach{
		        .blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE,
		        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		        .colorBlendOp = VK_BLEND_OP_ADD,
		        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		        .alphaBlendOp = VK_BLEND_OP_ADD,
		        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		const VkPipelineColorBlendStateCreateInfo colorBlend{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		        .attachmentCount = hasColorAttachment ? 1u : 0u,
		        .pAttachments = hasColorAttachment ? &colorBlendAttach : nullptr,
		};
		const VkDynamicState kBaseDynamicStates[] = {
		        VK_DYNAMIC_STATE_VIEWPORT,
		        VK_DYNAMIC_STATE_SCISSOR,
		};
		const VkDynamicState kDynamicStatesWithLineWidth[] = {
		        VK_DYNAMIC_STATE_VIEWPORT,
		        VK_DYNAMIC_STATE_SCISSOR,
		        VK_DYNAMIC_STATE_LINE_WIDTH,
		};
		const bool hasLineWidthDynamic = desc.lineWidthDynamic;
		const VkPipelineDynamicStateCreateInfo dynamicState{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		        .dynamicStateCount = hasLineWidthDynamic ? 3u : 2u,
		        .pDynamicStates = hasLineWidthDynamic ? kDynamicStatesWithLineWidth : kBaseDynamicStates,
		};

		// ── Pipeline layout ──────────────────────────────────────────────────
		VkPipelineLayout layout = VK_NULL_HANDLE;
		const VkPushConstantRange kModelRange{
		        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		        .offset = 0,
		        .size = sizeof(DrawContracts::PushConstants),
		};
		const VkPushConstantRange kCustomRange{
		        .stageFlags = vkPushConstantStages,
		        .offset = 0,
		        .size = desc.pushConstantSize,
		};
		const bool useCustomPush = desc.pushConstantSize > 0;
		std::vector<VkDescriptorSetLayout> vkSetLayouts;
		vkSetLayouts.reserve(desc.setLayouts.size());
		for (const gpu::DescriptorSetLayout setLayout: desc.setLayouts)
		{
			vkSetLayouts.push_back(static_cast<VkDescriptorSetLayout>(setLayout));
		}
		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .setLayoutCount = static_cast<std::uint32_t>(vkSetLayouts.size()),
		        .pSetLayouts = vkSetLayouts.data(),
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = useCustomPush ? &kCustomRange : &kModelRange,
		};
		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		{
			destroyModules();
			AE_UNEXPECTED(AetherError::Vulkan(0, "Failed to create pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo vertStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_VERTEX_BIT,
		        .module = vertModule,
		        .pName = vertEntry.c_str(),
		};
		const VkPipelineShaderStageCreateInfo fragStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .module = fragModule,
		        .pName = fragEntry.c_str(),
		};

		const uint32_t colorAttachmentCount = hasColorAttachment ? 1u : 0u;
		const VkFormat* pColorFormats = hasColorAttachment ? &vkColorFormat : nullptr;

		// ── The entry we will return. The 4 GPL libraries are filled in below
		//    and destroyed in unison with the linked pipeline by the registry.
		ResourceRegistry::PipelineEntry entry{};
		entry.device = device;
		entry.layout = layout;
		entry.ownsLayout = true;

		AE_ASSERT(layout != VK_NULL_HANDLE, "GraphicsPipeline: layout must be valid for GPL creation.");

		// ── GPL: vertex input interface library ──────────────────────────────
		const VkGraphicsPipelineLibraryCreateInfoEXT gplVertexInput{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
		        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
		};
		const VkGraphicsPipelineCreateInfo vertInputLibInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &gplVertexInput,
		        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
		        .pVertexInputState = &vertexInput,
		        .pInputAssemblyState = &inputAssembly,
		        .layout = layout,
		};
		VkPipeline vertInputLib = VK_NULL_HANDLE;
		VkResult result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &vertInputLibInfo, nullptr, &vertInputLib);
		if (result != VK_SUCCESS)
		{
			destroyModules();
			vkDestroyPipelineLayout(device, layout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create vertex-input GPL library."));
		}
		entry.vertInputLib = vertInputLib;

		// ── GPL: pre-rasterization library (vertex stage) ────────────────────
		const VkGraphicsPipelineLibraryCreateInfoEXT gplPreRaster{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
		        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
		};
		const VkGraphicsPipelineCreateInfo preRasterLibInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &gplPreRaster,
		        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
		        .stageCount = 1,
		        .pStages = &vertStage,
		        .pInputAssemblyState = &inputAssembly,
		        .pViewportState = &viewportState,
		        .pRasterizationState = &rasterizer,
		        .pDynamicState = &dynamicState,
		        .layout = layout,
		};
		VkPipeline preRasterLib = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &preRasterLibInfo, nullptr, &preRasterLib);
		if (result != VK_SUCCESS)
		{
			destroyModules();
			// Best-effort cleanup; the entry was never returned to the
			// registry so the registry won't double-free these.
			vkDestroyPipeline(device, vertInputLib, nullptr);
			vkDestroyPipelineLayout(device, layout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create pre-rasterization GPL library."));
		}
		entry.preRasterLib = preRasterLib;

		// ── GPL: fragment shader library ─────────────────────────────────────
		const VkPipelineRenderingCreateInfo fragShaderRenderingInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		        .colorAttachmentCount = colorAttachmentCount,
		        .pColorAttachmentFormats = pColorFormats,
		        .depthAttachmentFormat = vkDepthFormat,
		};
		const VkGraphicsPipelineLibraryCreateInfoEXT gplFragShader{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
		        .pNext = &fragShaderRenderingInfo,
		        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
		};
		const VkGraphicsPipelineCreateInfo fragShaderLibInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &gplFragShader,
		        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
		        .stageCount = 1,
		        .pStages = &fragStage,
		        .pDepthStencilState = &depthStencil,
		        .layout = layout,
		};
		VkPipeline fragShaderLib = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &fragShaderLibInfo, nullptr, &fragShaderLib);
		if (result != VK_SUCCESS)
		{
			destroyModules();
			vkDestroyPipeline(device, vertInputLib, nullptr);
			vkDestroyPipeline(device, preRasterLib, nullptr);
			vkDestroyPipelineLayout(device, layout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create fragment-shader GPL library."));
		}
		entry.fragShaderLib = fragShaderLib;

		// ── GPL: fragment output interface library ───────────────────────────
		const VkPipelineRenderingCreateInfo fragOutputRenderingInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		        .colorAttachmentCount = colorAttachmentCount,
		        .pColorAttachmentFormats = pColorFormats,
		        .depthAttachmentFormat = vkDepthFormat,
		};
		const VkGraphicsPipelineLibraryCreateInfoEXT gplFragOutput{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
		        .pNext = &fragOutputRenderingInfo,
		        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
		};
		const VkGraphicsPipelineCreateInfo fragOutputLibInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &gplFragOutput,
		        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
		        .pMultisampleState = &multisampling,
		        .pColorBlendState = &colorBlend,
		        .layout = layout,
		};
		VkPipeline fragOutputLib = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &fragOutputLibInfo, nullptr, &fragOutputLib);
		if (result != VK_SUCCESS)
		{
			destroyModules();
			vkDestroyPipeline(device, vertInputLib, nullptr);
			vkDestroyPipeline(device, preRasterLib, nullptr);
			vkDestroyPipeline(device, fragShaderLib, nullptr);
			vkDestroyPipelineLayout(device, layout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create fragment-output GPL library."));
		}
		entry.fragOutputLib = fragOutputLib;

		// ── GPL: link all libraries into final pipeline ──────────────────────
		const VkPipeline kLibs[] = {vertInputLib, preRasterLib, fragShaderLib, fragOutputLib};
		const VkPipelineLibraryCreateInfoKHR libLink{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
		        .libraryCount = 4,
		        .pLibraries = kLibs,
		};
		const VkPipelineRenderingCreateInfo renderingInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		        .pNext = &libLink,
		        .colorAttachmentCount = colorAttachmentCount,
		        .pColorAttachmentFormats = pColorFormats,
		        .depthAttachmentFormat = vkDepthFormat,
		};
		const VkGraphicsPipelineCreateInfo linkInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &renderingInfo,
		        .flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT,
		        .layout = layout,
		};

		VkPipeline pipeline = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &linkInfo, nullptr, &pipeline);

		destroyModules();

		if (result != VK_SUCCESS)
		{
			// Best-effort cleanup; entry never returned to registry.
			vkDestroyPipeline(device, vertInputLib, nullptr);
			vkDestroyPipeline(device, preRasterLib, nullptr);
			vkDestroyPipeline(device, fragShaderLib, nullptr);
			vkDestroyPipeline(device, fragOutputLib, nullptr);
			vkDestroyPipelineLayout(device, layout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to link GPL pipeline."));
		}
		entry.pipeline = pipeline;

		return entry;
	}
} // namespace aether::vkutil
